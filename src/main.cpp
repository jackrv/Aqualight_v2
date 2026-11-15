#include "main.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <time.h>
#include "telegram_ca.h"

namespace {
Schedule schedule;
bool fsReady = false;
bool botReady = false;
int64_t nextUpdate = 0;
uint32_t lastPoll = 0, pollInterval = 1500;
uint8_t brightness[4] = {};
BearSSL::X509List trust(TELEGRAM_CA);

bool clockReady() { return time(nullptr) > 1700000000; }

String scheduleJson(const Schedule& value) {
    DynamicJsonDocument doc(4096);
    for (uint8_t c = 0; c < 4; ++c) {
        JsonArray points = doc.createNestedArray("ch" + String(c));
        for (uint8_t i = 0; i < value.channels[c].count; ++i) {
            JsonObject p = points.createNestedObject();
            p["t"] = value.channels[c].points[i].t;
            p["v"] = value.channels[c].points[i].v;
        }
    }
    String result;
    serializeJson(doc, result);
    return result;
}

bool loadSchedule(const char* path) {
    File file = LittleFS.open(path, "r");
    if (!file || file.size() > MAX_SCHEDULE_BYTES) return false;
    DynamicJsonDocument doc(4096);
    auto error = deserializeJson(doc, file);
    Schedule candidate;
    if (error || !parseSchedule(doc.as<JsonVariantConst>(), candidate)) return false;
    schedule = candidate;
    return true;
}

bool saveSchedule(const Schedule& candidate) {
    if (!fsReady) return false;
    String json = scheduleJson(candidate);
    File file = LittleFS.open("/schedule.tmp", "w");
    if (!file) return false;
    size_t written = file.print(json);
    file.flush();
    file.close();
    if (written != json.length()) { LittleFS.remove("/schedule.tmp"); return false; }
    // LittleFS rename replaces the destination atomically; the old file survives
    // an interrupted write. The temporary file is never used on startup.
    if (!LittleFS.rename("/schedule.tmp", "/schedule.json")) return false;
    schedule = candidate;
    return true;
}

void updateLight() {
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    uint32_t second = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    for (uint8_t c = 0; c < 4; ++c) {
        uint8_t value = 0;
        if (clockReady()) value = interpolate(schedule.channels[c], second);
        brightness[c] = value;
        analogWrite(PWM_PINS[c], value);
    }
}

// Raw Bot API JSON preserves service fields such as message.web_app_data.
// HTTP/1.0 avoids chunked encoding when ArduinoJson reads the response stream.
bool api(const char* method, const String& payload, JsonDocument& response) {
    BearSSL::WiFiClientSecure client;
    client.setTrustAnchors(&trust);
    client.setTimeout(4000);
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(4000);
    if (!http.begin(client, String("https://api.telegram.org/bot") + BOT_TOKEN + "/" + method)) return false;
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    bool ok = false;
    if (code > 0) {
        // sendMessage echoes the complete keyboard URL. Retain only the fields
        // we need, so a successful send cannot exhaust its small response buffer.
        DynamicJsonDocument filter(1024);
        filter["ok"] = true;
        filter["error_code"] = true;
        filter["parameters"]["retry_after"] = true;
        if (strcmp(method, "getUpdates") == 0) {
            JsonObject item = filter.createNestedArray("result").createNestedObject();
            item["update_id"] = true;
            JsonObject message = item.createNestedObject("message");
            message["chat"]["id"] = true;
            message["chat"]["type"] = true;
            message["from"]["id"] = true;
            message["text"] = true;
            message["web_app_data"]["data"] = true;
        }
        auto error = deserializeJson(response, http.getStream(),
            DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(20));
        ok = !error && code == 200 && response["ok"].as<bool>();
        if (!ok && response["parameters"]["retry_after"].is<uint32_t>()) {
            pollInterval = min<uint32_t>(response["parameters"]["retry_after"].as<uint32_t>(), 3600) * 1000UL;
        }
    }
    http.end();
    updateLight();
    return ok;
}

String urlEncode(const String& input) {
    const char hex[] = "0123456789ABCDEF";
    String out;
    out.reserve(input.length() * 3);
    for (size_t i = 0; i < input.length(); ++i) {
        uint8_t c = input[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

void sendMessage(const String& text, bool editor = false) {
    String payload;
    {
        DynamicJsonDocument request(8192);
        request["chat_id"] = JACKRV_ID;
        request["text"] = text;
        if (editor) {
            String url = WEBAPP_URL;
            url += url.indexOf('?') < 0 ? "?data=" : "&data=";
            url += urlEncode(scheduleJson(schedule));
            JsonObject markup = request.createNestedObject("reply_markup");
            markup["resize_keyboard"] = true;
            JsonArray row = markup.createNestedArray("keyboard").createNestedArray();
            JsonObject button = row.createNestedObject();
            button["text"] = "💡 Редактор освещения";
            button["web_app"]["url"] = url;
        }
        serializeJson(request, payload);
    }
    DynamicJsonDocument response(1024);
    if (!api("sendMessage", payload, response)) Serial.println(F("[Telegram] sendMessage failed"));
}

void handleMessage(JsonObjectConst message) {
    // Restrict both the sender and private chat. web_app_data is untrusted input.
    String owner = JACKRV_ID;
    if (message["chat"]["type"] != "private" ||
        message["chat"]["id"].as<String>() != owner ||
        message["from"]["id"].as<String>() != owner) return;
    const char* data = message["web_app_data"]["data"];
    String text = message["text"] | "";
    if (data || text.startsWith("{")) {
        const char* json = data ? data : text.c_str();
        if (strlen(json) > MAX_SCHEDULE_BYTES) { sendMessage("График слишком большой."); return; }
        DynamicJsonDocument doc(4096);
        Schedule candidate;
        auto error = deserializeJson(doc, json);
        if (error || !parseSchedule(doc.as<JsonVariantConst>(), candidate)) {
            sendMessage("Неверный график: нужны ch0…ch3, 2–12 точек, t=0…1440, v=0…255. Время строго возрастает, крайние точки 0 и 1440 с одинаковой яркостью.");
        } else if (!saveSchedule(candidate)) {
            sendMessage("Ошибка записи LittleFS. Старый график сохранён.");
        } else {
            updateLight();
            sendMessage("✅ График сохранён. Кнопка обновлена актуальными данными.", true);
        }
    } else if (text == "/status") {
        char stamp[48] = "ожидается NTP";
        if (clockReady()) {
            time_t now = time(nullptr); tm local{}; localtime_r(&now, &local);
            strftime(stamp, sizeof(stamp), "%d.%m.%Y %H:%M:%S %Z", &local);
        }
        String status = String("Время: ") + stamp + "\nPWM W/R/G/B: ";
        for (uint8_t c = 0; c < 4; ++c) { if (c) status += "/"; status += brightness[c]; }
        status += String("\nLittleFS: ") + (fsReady ? "OK" : "ошибка") + "\nСвободно RAM: " + ESP.getFreeHeap();
        sendMessage(status);
    } else if (text == "/schedule") {
        sendMessage(scheduleJson(schedule));
    } else {
        sendMessage("Управление светом аквариума\n/editor — открыть график\n/status — время и яркость\n/schedule — текущий JSON\nПосле сохранения открывайте редактор через обновлённую кнопку.", true);
    }
}

void pollBot() {
    if (!botReady) {
        DynamicJsonDocument response(512);
        // A webhook must be removed before getUpdates. Preserve queued updates.
        botReady = api("deleteWebhook", "{\"drop_pending_updates\":false}", response);
        if (!botReady) return;
    }
    DynamicJsonDocument request(256);
    request["offset"] = nextUpdate;
    request["limit"] = 1;
    request["timeout"] = 0;
    request.createNestedArray("allowed_updates").add("message");
    String payload; serializeJson(request, payload);
    DynamicJsonDocument response(8192);
    if (!api("getUpdates", payload, response)) {
        pollInterval = min<uint32_t>(pollInterval * 2, 60000);
        Serial.println(F("[Telegram] polling failed (network, TLS or API)"));
        return;
    }
    pollInterval = 1500;
    for (JsonObjectConst update : response["result"].as<JsonArrayConst>()) {
        int64_t id = update["update_id"].as<int64_t>();
        if (id >= nextUpdate) {
            handleMessage(update["message"].as<JsonObjectConst>());
            nextUpdate = id + 1;
        }
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    analogWriteRange(255);
    analogWriteFreq(1000);
    for (uint8_t pin : PWM_PINS) { pinMode(pin, OUTPUT); analogWrite(pin, 0); }
    // Safe initial schedule: dark until an owner explicitly saves a graph.
    for (Channel& ch : schedule.channels) { ch.count = 2; ch.points[0] = {0, 0}; ch.points[1] = {1440, 0}; }
    LittleFSConfig fsConfig;
    fsConfig.setAutoFormat(false);
    LittleFS.setConfig(fsConfig);
    fsReady = LittleFS.begin();
    if (fsReady) {
        if (!loadSchedule("/schedule.json")) Serial.println(F("[FS] No valid schedule; lights off"));
    } else Serial.println(F("[FS] Mount failed; uploadfs required for first setup"));
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    configTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    Serial.println(F("[Boot] Waiting for Wi-Fi and NTP without blocking PWM"));
}

void loop() {
    static uint32_t lastLight = 0;
    if (millis() - lastLight >= 250) { lastLight = millis(); updateLight(); }
    if (WiFi.status() == WL_CONNECTED && clockReady() && millis() - lastPoll >= pollInterval) {
        pollBot();
        lastPoll = millis();
    }
    delay(5);
}
