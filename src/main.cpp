#include "main.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <time.h>
#include "telegram_ca.h"
#include "ota_policy.h"
#include "telegram_json.h"
#include <Updater.h>
#include <stdarg.h>

namespace {
Schedule schedule;
bool fsReady = false;
bool botReady = false;
int64_t nextUpdate = 0;
uint32_t lastPoll = 0, pollInterval = 1500;
uint8_t brightness[4] = {};
BearSSL::X509List trust(TELEGRAM_CA);
OtaWindow otaWindow;
struct PendingFirmware {
    String fileId;
    size_t size = 0;
    char md5[33] = {};
} pendingFirmware;

// Frame each diagnostic as one numbered CRLF-terminated record. Repeated
// sequence numbers in a monitor indicate display/capture duplication.
void logLine(PGM_P format, ...) {
    static uint32_t sequence = 0;
    char line[256];
    int prefix = snprintf(line, sizeof(line), "[%lu %lums] ",
        static_cast<unsigned long>(++sequence), millis());
    va_list args;
    va_start(args, format);
    vsnprintf_P(line + prefix, sizeof(line) - prefix - 2, format, args);
    va_end(args);
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) --length;
    line[length++] = '\r';
    line[length++] = '\n';
    Serial.write(reinterpret_cast<const uint8_t*>(line), length);
}

size_t otaCapacity() {
    uint32_t free = ESP.getFreeSketchSpace();
    return free > 4096 ? (free - 4096) & ~size_t(4095) : 0;
}

bool clockReady() { return time(nullptr) > 1700000000; }

bool tlsMemoryReady() {
    if (ESP.getFreeHeap() >= 38000 && ESP.getMaxFreeBlockSize() >= 18000) return true;
    logLine(PSTR("[TLS] Request postponed: heap=%u, largest block=%u\n"),
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    return false;
}

void logConnectivity() {
    static int previousStatus = -1;
    static bool previousClock = false;
    static uint32_t lastReport = 0;
    int status = WiFi.status();
    bool ready = clockReady();
    bool changed = status != previousStatus || ready != previousClock;
    if (!changed && (ready && status == WL_CONNECTED)) return;
    if (!changed && millis() - lastReport < 10000) return;
    lastReport = millis();
    int oldStatus = previousStatus;
    bool oldClock = previousClock;
    previousStatus = status;
    previousClock = ready;
    if (status != WL_CONNECTED) {
        const char* reason = "connecting";
        if (status == WL_NO_SSID_AVAIL) reason = "SSID not found";
        else if (status == WL_CONNECT_FAILED) reason = "connection failed; check Wi-Fi settings";
        else if (status == WL_DISCONNECTED) reason = "disconnected";
        logLine(PSTR("[WiFi] %s (status=%d, uptime=%lu s)\n"), reason, status, millis() / 1000UL);
    } else {
        if (status != oldStatus) {
            logLine(PSTR("[WiFi] Connected; IP=%s, RSSI=%d dBm\n"),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }
        if (!ready) {
            logLine(PSTR("[NTP] Waiting for time; check internet access, DNS and UDP port 123"));
        } else if (!oldClock || status != oldStatus) {
            time_t now = time(nullptr);
            tm local{}; localtime_r(&now, &local);
            char stamp[48];
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %Z", &local);
            logLine(PSTR("[NTP] Time ready: %s; Telegram polling enabled\n"), stamp);
        }
    }
}

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
bool api(const char* method, const String& payload, DynamicJsonDocument& response) {
    // The large polling response must not occupy RAM during the TLS handshake.
    const size_t responseCapacity = response.capacity();
    response = DynamicJsonDocument(0);
    if (!tlsMemoryReady()) return false;
    BearSSL::WiFiClientSecure client;
    client.setTrustAnchors(&trust);
    // Full-size receive records are required without negotiated MFLN.
    client.setBufferSizes(16384, 512);
    client.setTimeout(15000);
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(15000);
    if (!http.begin(client, String(F("https://api.telegram.org/bot")) + BOT_TOKEN + "/" + method)) return false;
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    bool ok = false;
    if (code > 0) {
        response = DynamicJsonDocument(responseCapacity);
        // sendMessage echoes the complete keyboard URL. Retain only the fields
        // we need, so a successful send cannot exhaust its small response buffer.
        DynamicJsonDocument filter(1536);
        fillTelegramFilter(filter, method);
        auto error = deserializeJson(response, http.getStream(),
            DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(20));
        ok = !error && code == 200 && response["ok"].as<bool>();
        if (!ok) {
            logLine(PSTR("[Telegram] %s failed: HTTP=%d, JSON=%s, API=%d\n"),
                method, code, error.c_str(), response["error_code"] | 0);
            if (error == DeserializationError::NoMemory) {
                logLine(PSTR("[JSON] requested=%u, capacity=%u, used=%u, heap=%u, largest block=%u"),
                    unsigned(responseCapacity), unsigned(response.capacity()), unsigned(response.memoryUsage()),
                    ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
            }
        }
        if (!ok && response["parameters"]["retry_after"].is<uint32_t>()) {
            pollInterval = min<uint32_t>(response["parameters"]["retry_after"].as<uint32_t>(), 3600) * 1000UL;
        }
    } else {
        // Numeric diagnostics only; never print the request URL containing token.
        logLine(PSTR("[Telegram] %s failed: transport=%d, TLS=%d\n"),
            method, code, client.getLastSSLError());
    }
    http.end();
    client.stop();
    response.shrinkToFit();
    if (!ok) logLine(PSTR("[Heap] free=%u, largest block=%u\n"),
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
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
        request["chat_id"] = TELEGRAM_OWNER_ID;
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
    if (!api("sendMessage", payload, response)) logLine(PSTR("[Telegram] sendMessage failed"));
}

void queueFirmware(JsonObjectConst message) {
    if (!otaWindow.active(millis())) {
        sendMessage(F("Для обновления сначала отправьте /update, затем firmware.bin с подписью md5:… в течение 5 минут."));
        return;
    }
    String filename = message["document"]["file_name"] | "";
    filename.toLowerCase();
    String fileId = message["document"]["file_id"] | "";
    size_t size = message["document"]["file_size"] | size_t(0);
    char md5[33];
    if (!filename.endsWith(".bin") || size < 4096 || size > otaCapacity() ||
        fileId.isEmpty() || fileId.length() > 256 ||
        !parseFirmwareMd5(message["caption"] | "", md5)) {
        sendMessage(String(F("Нужен .bin для AquaLight D1 mini с подписью md5:32_шестнадцатеричных_символа. Размер 4096…")) + String(otaCapacity()) + " байт. Контрольная сумма выводится при сборке.");
        return;
    }
    pendingFirmware.fileId = fileId;
    pendingFirmware.size = size;
    memcpy(pendingFirmware.md5, md5, sizeof(md5));
    otaWindow.cancel();
}

// Desktop sends a bot-owned document; the owner's button click arrives as
// callback_query even though the original outgoing document is not an update.
void handleFirmwareButton(JsonObjectConst query) {
    JsonObjectConst source = query["message"].as<JsonObjectConst>();
    String owner = TELEGRAM_OWNER_ID;
    String botId = BOT_TOKEN;
    botId = botId.substring(0, botId.indexOf(':'));
    String data = query["data"] | "";
    String caption = source["caption"] | "";
    char md5[33];
    bool authorized = query["from"]["id"].as<String>() == owner &&
        source["chat"]["type"] == "private" && source["chat"]["id"].as<String>() == owner &&
        source["from"]["id"].as<String>() == botId &&
        parseFirmwareMd5(caption.c_str(), md5) && data == String(F("ota:")) + md5;
    {
        DynamicJsonDocument request(384), response(512);
        request["callback_query_id"] = query["id"];
        request["text"] = authorized ? "Проверяю файл прошивки…" : "Кнопка недоступна.";
        String payload; serializeJson(request, payload);
        api("answerCallbackQuery", payload, response);
    }
    if (!authorized) return;
    if (ESP.getSketchMD5().equalsIgnoreCase(md5)) {
        sendMessage(F("Эта прошивка уже установлена."));
        return;
    }
    otaWindow.arm(millis());
    queueFirmware(source);
    otaWindow.cancel();
}

// No JSON polling document is retained during download. Only a small buffer,
// TLS buffers and the core Updater's sector buffer occupy RAM.
bool downloadFirmware(const String& path, size_t size, const char* md5, String& error) {
    if (!tlsMemoryReady()) {
        error = F("Недостаточно свободной памяти для HTTPS. Повторите после перезапуска.");
        return false;
    }
    bool complete = false;
    {
        BearSSL::WiFiClientSecure client;
        client.setTrustAnchors(&trust);
        client.setTimeout(15000);
        HTTPClient http;
        http.useHTTP10(true);
        http.setTimeout(15000);
        if (!http.begin(client, String(F("https://api.telegram.org/file/bot")) + BOT_TOKEN + "/" + path)) {
            error = F("Не удалось открыть HTTPS.");
            return false;
        }
        int code = http.GET();
        if (code != HTTP_CODE_OK || http.getSize() != int(size)) {
            error = F("Ошибка HTTP или размер файла не совпадает с Telegram.");
            http.end();
            return false;
        }
        if (!Update.begin(size, U_FLASH)) {
            error = Update.getErrorString();
            http.end();
            return false;
        }
        if (!Update.setMD5(md5)) {
            error = F("Неверная MD5.");
            Update.end();
            http.end();
            return false;
        }
        FirmwareMarkerScanner marker;
        uint8_t buffer[1024];
        size_t received = 0;
        uint32_t started = millis(), lastRead = started;
        auto& stream = http.getStream();
        while (received < size) {
            if (millis() - started >= 180000 || millis() - lastRead >= 15000 ||
                WiFi.status() != WL_CONNECTED) {
                error = F("Загрузка прервана или истёк тайм-аут.");
                break;
            }
            int available = stream.available();
            if (available <= 0) {
                if (!http.connected()) { error = F("Неполный файл."); break; }
                updateLight(); delay(5); continue;
            }
            size_t amount = min<size_t>(sizeof(buffer), min<size_t>(available, size - received));
            size_t count = stream.readBytes(buffer, amount);
            if (!count) { updateLight(); delay(5); continue; }
            if (received == 0 && buffer[0] != 0xE9) {
                error = F("Это не несжатый образ прошивки ESP8266.");
                break;
            }
            marker.add(buffer, count);
            // Withhold the last block of a foreign image. end(false) cannot
            // finalize an incomplete download, even when its MD5 is correct.
            if (received + count == size && !marker.valid()) {
                error = F("В файле нет метки прошивки AquaLight D1 mini.");
                break;
            }
            if (Update.write(buffer, count) != count) {
                error = Update.getErrorString();
                break;
            }
            received += count;
            lastRead = millis();
            updateLight();
            delay(1);
        }
        http.end();
        complete = received == size && marker.valid();
        if (!complete) {
            if (error.isEmpty()) error = F("В файле нет метки прошивки AquaLight D1 mini.");
            // Incomplete writes or Updater errors cannot schedule a boot copy.
            Update.end(false);
        }
    } // Release TLS before final verification and before Telegram notifications.
    if (!complete) return false;
    if (!Update.end(false)) { error = Update.getErrorString(); return false; }
    return true;
}

void processFirmware() {
    if (pendingFirmware.fileId.isEmpty()) return;
    PendingFirmware firmware = pendingFirmware;
    pendingFirmware.fileId = "";
    String path;
    {
        // Confirm consumption BEFORE reboot, so getUpdates cannot replay the
        // document. Do not consume any newer update returned by this request.
        DynamicJsonDocument ack(4096);
        String request = "{\"offset\":" + String(static_cast<long long>(nextUpdate)) +
                         ",\"limit\":1,\"timeout\":0,\"allowed_updates\":[\"message\",\"callback_query\"]}";
        if (!api("getUpdates", request, ack)) {
            sendMessage(F("Обновление отменено: Telegram не подтвердил обработку файла. Начните с /update."));
            return;
        }
    }
    {
        DynamicJsonDocument request(512), response(1024);
        request["file_id"] = firmware.fileId;
        String payload; serializeJson(request, payload);
        if (!api("getFile", payload, response) ||
            response["result"]["file_size"].as<size_t>() != firmware.size) {
            sendMessage(F("Не удалось получить файл прошивки. Начните с /update."));
            return;
        }
        path = response["result"]["file_path"] | "";
    }
    if (path.isEmpty() || path.startsWith("/") || path.indexOf("..") >= 0 || path.indexOf(':') >= 0) {
        sendMessage(F("Telegram вернул недопустимый путь файла."));
        return;
    }
    sendMessage(F("⏳ Загружаю и проверяю прошивку. График LittleFS сохранится. После успешной записи плата перезагрузится."));
    String error;
    if (!downloadFirmware(path, firmware.size, firmware.md5, error)) {
        sendMessage("❌ Обновление не установлено: " + error + "\nРаботает прежняя прошивка. Для повторной попытки: /update.");
        return;
    }
    sendMessage(F("✅ Прошивка проверена и записана. Перезагрузка… После подключения проверьте /status."));
    delay(300);
    ESP.restart();
}

void handleMessage(JsonObjectConst message) {
    // Restrict both the sender and private chat. web_app_data is untrusted input.
    String owner = TELEGRAM_OWNER_ID;
    if (message["chat"]["type"] != "private" ||
        message["chat"]["id"].as<String>() != owner ||
        message["from"]["id"].as<String>() != owner) return;
    const char* data = message["web_app_data"]["data"];
    String text = message["text"] | "";
    if (text == "/update") {
        otaWindow.arm(millis());
        sendMessage(String(F("Обновление разрешено на 5 минут. Отправьте firmware.bin как документ с подписью md5:… из файла firmware.bin.md5. Отправка запускает установку и перезагрузку. /cancel — отмена. Доступно OTA: ")) + String(otaCapacity()) + " байт.");
    } else if (text == "/cancel") {
        otaWindow.cancel();
        pendingFirmware.fileId = "";
        sendMessage(F("Ожидание прошивки отменено."));
    } else if (message["document"].is<JsonObjectConst>()) {
        queueFirmware(message);
    } else if (data || text.startsWith("{")) {
        const char* json = data ? data : text.c_str();
        if (strlen(json) > MAX_SCHEDULE_BYTES) { sendMessage(F("График слишком большой.")); return; }
        DynamicJsonDocument doc(4096);
        Schedule candidate;
        auto error = deserializeJson(doc, json);
        bool valid = !error && parseSchedule(doc.as<JsonVariantConst>(), candidate);
        doc.clear();
        doc.shrinkToFit();
        if (!valid) {
            sendMessage(F("Неверный график: нужны ch0…ch3, 2–12 точек, t=0…1440, v=0…255. Время строго возрастает, крайние точки 0 и 1440 с одинаковой яркостью."));
        } else if (!saveSchedule(candidate)) {
            sendMessage(F("Ошибка записи LittleFS. Старый график сохранён."));
        } else {
            updateLight();
            sendMessage(F("✅ График сохранён. Кнопка обновлена актуальными данными."), true);
        }
    } else if (text == "/status") {
        char stamp[48] = "ожидается NTP";
        if (clockReady()) {
            time_t now = time(nullptr); tm local{}; localtime_r(&now, &local);
            strftime(stamp, sizeof(stamp), "%d.%m.%Y %H:%M:%S %Z", &local);
        }
        String status = String(F("Время: ")) + stamp + "\nPWM W/R/G/B: ";
        for (uint8_t c = 0; c < 4; ++c) { if (c) status += "/"; status += brightness[c]; }
        status += String(F("\nLittleFS: ")) + (fsReady ? "OK" : "ошибка") + "\nСвободно RAM: " + ESP.getFreeHeap();
        status += String(F("\nСборка: ")) + __DATE__ + " " + __TIME__ +
                  "\n" + FIRMWARE_MARKER + "\nOTA: " + otaCapacity() + " байт";
        sendMessage(status);
    } else if (text == "/schedule") {
        sendMessage(scheduleJson(schedule));
    } else {
        sendMessage(F("Управление светом аквариума\n/editor — открыть график\n/status — время и яркость\n/schedule — текущий JSON\n/update — обновить прошивку\nПосле сохранения открывайте редактор через обновлённую кнопку."), true);
    }
}

void pollBot() {
    if (!botReady) {
        DynamicJsonDocument response(512);
        // A webhook must be removed before getUpdates. Preserve queued updates.
        botReady = api("deleteWebhook", "{\"drop_pending_updates\":false}", response);
        if (!botReady) {
            pollInterval = min<uint32_t>(pollInterval * 2, 60000);
            return;
        }
    }
    DynamicJsonDocument request(256);
    request["offset"] = nextUpdate;
    request["limit"] = 1;
    request["timeout"] = 0;
    JsonArray allowed = request.createNestedArray("allowed_updates");
    allowed.add("message");
    allowed.add("callback_query");
    String payload; serializeJson(request, payload);
    DynamicJsonDocument response(4096);
    if (!api("getUpdates", payload, response)) {
        pollInterval = min<uint32_t>(pollInterval * 2, 60000);
        logLine(PSTR("[Telegram] polling failed (network, TLS or API)"));
        return;
    }
    static bool reportedReady = false;
    if (!reportedReady) {
        logLine(PSTR("[Telegram] Ready. Send /start to the bot from the owner account."));
        reportedReady = true;
    }
    pollInterval = 1500;
    for (JsonObjectConst update : response["result"].as<JsonArrayConst>()) {
        int64_t id = update["update_id"].as<int64_t>();
        if (id >= nextUpdate) {
            if (update["callback_query"].is<JsonObjectConst>())
                handleFirmwareButton(update["callback_query"].as<JsonObjectConst>());
            else handleMessage(update["message"].as<JsonObjectConst>());
            nextUpdate = id + 1;
        }
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    logLine(PSTR("[Boot] Build %s %s"), __DATE__, __TIME__);
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
        if (!loadSchedule("/schedule.json")) logLine(PSTR("[FS] No valid schedule; lights off"));
    } else logLine(PSTR("[FS] Mount failed; uploadfs required for first setup"));
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    configTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    logLine(PSTR("[Heap] boot free=%u, largest block=%u\n"),
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    logLine(PSTR("[Boot] Waiting for Wi-Fi and NTP without blocking PWM"));
}

void loop() {
    logConnectivity();
    static uint32_t lastLight = 0;
    if (millis() - lastLight >= 250) { lastLight = millis(); updateLight(); }
    if (!pendingFirmware.fileId.isEmpty()) {
        processFirmware();
        lastPoll = millis();
    }
    if (WiFi.status() == WL_CONNECTED && clockReady() && millis() - lastPoll >= pollInterval) {
        pollBot();
        lastPoll = millis();
    }
    delay(5);
}
