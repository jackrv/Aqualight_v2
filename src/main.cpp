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
#include <MD5Builder.h>
#include <memory>
#include <new>
#include <ESP8266WebServer.h>
#include <osapi.h>
#include <GyverDS18.h>
#include <coredecls.h>
#include <sys/time.h>
#include "recovery.h"
#include "default_schedule.h"

namespace {
Schedule schedule;
bool fsReady = false;
bool scheduleLoaded = false;
bool scheduleFromFirmware = false;   // the starting schedule was applied on this boot
bool telegramOnline = false;
bool botReady = false;
int64_t nextUpdate = 0;
uint32_t lastPoll = 0, pollInterval = 1500;
// Long polling: Telegram holds the request until an update arrives. The wait
// must stay below the 15 s HTTP timeout; the light is refreshed from yield().
constexpr uint8_t LONG_POLL_SECONDS = 10;
constexpr uint32_t POLL_GAP_MS = 100, POLL_RETRY_MS = 1500;
int64_t oversizedUpdate = -1;
uint8_t brightness[4] = {};      // schedule units 0…255, for /status
uint16_t fineLevel[4] = {};      // the same level in 1/64 steps, drives the PWM
bool ntpSynced = false;
// A raw SDK timer: <Ticker.h> pulls in the core's <Schedule.h>, which on a
// case-insensitive file system resolves to this project's schedule.h.
os_timer_t lightTimer;
// eboot keeps its OTA command in the first RTC user blocks; stay clear of it.
constexpr uint32_t SAVED_CLOCK_RTC_OFFSET = 64;
constexpr char SETUP_AP_NAME[] = "AquaLight-Setup";
String wifiSsid, wifiPass;
GyverDS18Single waterSensor(WATER_TEMP_PIN);
constexpr uint32_t WATER_TEMP_PERIOD_MS = 10000, WATER_TEMP_STALE_MS = 60000;
float waterTemp = 0;
uint32_t waterTempAt = 0;
bool waterTempKnown = false;
std::unique_ptr<ESP8266WebServer> portal;
uint32_t lastWifiOk = 0, restartAt = 0;
bool restartPending = false, stationPaused = false;
BearSSL::X509List trust(TELEGRAM_CA);
BearSSL::Session telegramSession;
std::unique_ptr<BearSSL::WiFiClientSecure> telegramConnection;
OtaWindow otaWindow;
String otaState;
int32_t otaMessageId = 0;
bool otaMessageIsCaption = false;
char expectedBootMd5[33] = {};
uint32_t lastBootReport = 0;
bool bootReportAttempted = false;
bool bootAnnounced = false;   // one boot message per start: greeting or OTA result
int64_t callbackCheckpoint = 0;
String cachedFirmwareMd5;
uint64_t uptimeMs = 0;
uint32_t lastUptimeTick = 0;
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

String formatDateTime(const tm& local) {
    static const char months[12][17] PROGMEM = {
        "января", "февраля", "марта", "апреля", "мая", "июня",
        "июля", "августа", "сентября", "октября", "ноября", "декабря"
    };
    char month[17], text[96];
    strcpy_P(month, months[local.tm_mon]);
    snprintf_P(text, sizeof(text), PSTR("%d %s %d, %02d:%02d:%02d"),
        local.tm_mday, month, local.tm_year + 1900,
        local.tm_hour, local.tm_min, local.tm_sec);
    return String(text);
}

String formatKyivTime(time_t now) {
    if (!clockReady()) return String(F("ожидается синхронизация времени"));
    tm local{};
    localtime_r(&now, &local);
    return formatDateTime(local);
}

tm buildTime() {
    tm build{};
    char month[4];
    int year = 1900;
    sscanf(__DATE__, "%3s %d %d", month, &build.tm_mday, &year);
    const char* monthCodes = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* found = strstr(monthCodes, month);
    build.tm_mon = found ? (found - monthCodes) / 3 : 0;
    build.tm_year = year - 1900;
    sscanf(__TIME__, "%d:%d:%d", &build.tm_hour, &build.tm_min, &build.tm_sec);
    return build;
}

String formatBuildTime() { return formatDateTime(buildTime()); }

// "16 ноя 14:32" for /status, where every line has to earn its place.
String formatShortDateTime(const tm& local, bool seconds) {
    static const char months[12][7] PROGMEM = {
        "янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек"
    };
    char month[7], text[40];
    strcpy_P(month, months[local.tm_mon]);
    int length = snprintf_P(text, sizeof(text), PSTR("%d %s %02d:%02d"),
        local.tm_mday, month, local.tm_hour, local.tm_min);
    if (seconds) snprintf_P(text + length, sizeof(text) - length, PSTR(":%02d"), local.tm_sec);
    return String(text);
}

void trackUptime() {
    uint32_t now = millis();
    uptimeMs += uint32_t(now - lastUptimeTick);
    lastUptimeTick = now;
}

// Two most significant units are enough: "3д 4ч", "4ч 12м", "12м 5с".
String formatUptime() {
    trackUptime();
    uint32_t seconds = uptimeMs / 1000;
    unsigned long days = seconds / 86400;
    unsigned hours = seconds / 3600 % 24, minutes = seconds / 60 % 60;
    char text[40];
    if (days) snprintf_P(text, sizeof(text), PSTR("%luд %uч"), days, hours);
    else if (hours) snprintf_P(text, sizeof(text), PSTR("%uч %uм"), hours, minutes);
    else snprintf_P(text, sizeof(text), PSTR("%uм %uс"), minutes, unsigned(seconds % 60));
    return String(text);
}

bool resetWasAbnormal() {
    uint32_t reason = ESP.getResetInfoPtr()->reason;
    return reason == REASON_WDT_RST || reason == REASON_EXCEPTION_RST || reason == REASON_SOFT_WDT_RST;
}

const __FlashStringHelper* resetReasonLabel() {
    switch (ESP.getResetInfoPtr()->reason) {
        case REASON_DEFAULT_RST: return F("включение питания");
        case REASON_WDT_RST: return F("аппаратный watchdog");
        case REASON_EXCEPTION_RST: return F("ошибка программы");
        case REASON_SOFT_WDT_RST: return F("программный watchdog");
        case REASON_SOFT_RESTART: return F("программный перезапуск");
        case REASON_DEEP_SLEEP_AWAKE: return F("выход из сна");
        case REASON_EXT_SYS_RST: return F("внешний сброс / Reset");
        default: return F("неизвестна");
    }
}

void logMemory(const char* stage) {
    logLine(PSTR("[Memory] %s: heap=%u, largest=%u, stack=%u"), stage,
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getFreeContStack());
}

bool tlsMemoryReady(bool compact) {
    uint32_t required = compact ? 26000 : 38000;
    uint32_t block = compact ? 9000 : 22000;
    if (ESP.getFreeHeap() >= required && ESP.getMaxFreeBlockSize() >= block) return true;
    logLine(PSTR("[TLS] Request postponed: need=%u, heap=%u, largest=%u"),
        required, ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    return false;
}

std::unique_ptr<BearSSL::WiFiClientSecure> openTelegramTls(const char* stage, String& error) {
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
        bool compact = attempt == 0;
        if (!tlsMemoryReady(compact)) {
            error = F("Недостаточно свободной памяти для TLS. Обновление не начато.");
            return nullptr;
        }
        logMemory(stage);
        std::unique_ptr<BearSSL::WiFiClientSecure> client(new (std::nothrow) BearSSL::WiFiClientSecure());
        if (!client) { error = F("Не удалось выделить память для HTTPS."); return nullptr; }
        client->setTrustAnchors(&trust);
        client->setSession(&telegramSession);
        client->setBufferSizes(compact ? 4096 : 16384, 512);
        client->setTimeout(15000);
        delay(1);
        bool connected = client->connect("api.telegram.org", 443);
        if (connected && (!compact || client->getMFLNStatus())) {
            logLine(PSTR("[TLS] Connected: RX=%u, MFLN=%d"), compact ? 4096U : 16384U, client->getMFLNStatus());
            logMemory("TLS ready");
            return client;
        }
        logLine(PSTR("[TLS] Connection failed or MFLN unavailable: compact=%d, TLS=%d"),
            compact, client->getLastSSLError());
        client->stop();
        client.reset();
        delay(25);
        // No HTTP request has been sent. Retrying with a full buffer cannot
        // duplicate an API action. Never use a short buffer without MFLN.
    }
    error = F("Не удалось установить защищённое соединение с Telegram.");
    return nullptr;
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
            String stamp = formatKyivTime(time(nullptr));
            logLine(PSTR("[NTP] Time ready: %s; Telegram polling enabled"), stamp.c_str());
        }
    }
}

void updateLight();

// The core getSketchMD5() hashes the whole image without yielding. Do it in
// small blocks so a subsequent TLS handshake does not exhaust watchdog time.
String runningFirmwareMd5() {
    if (cachedFirmwareMd5.length() == 32) return cachedFirmwareMd5;
    MD5Builder md5;
    md5.begin();
    uint32_t buffer[128];
    uint32_t size = ESP.getSketchSize();
    if (!size) return String();
    for (uint32_t offset = 0; offset < size; offset += sizeof(buffer)) {
        size_t amount = min<size_t>(sizeof(buffer), size - offset);
        if (!ESP.flashRead(offset, buffer, (amount + 3) & ~size_t(3))) return String();
        md5.add(reinterpret_cast<uint8_t*>(buffer), amount);
        updateLight();
        delay(1);
    }
    md5.calculate();
    cachedFirmwareMd5 = md5.toString();
    return cachedFirmwareMd5;
}

// Persist only callbacks in flight, not a permanent Telegram offset. After the
// next successful getUpdates confirms that offset, remove the checkpoint.
bool saveCallbackCheckpoint(int64_t offset) {
    if (!fsReady) return false;
    String value = String(static_cast<long long>(offset));
    File file = LittleFS.open("/telegram-inflight.tmp", "w");
    if (!file) return false;
    size_t written = file.print(value);
    file.flush(); file.close();
    if (written != value.length() ||
        !LittleFS.rename("/telegram-inflight.tmp", "/telegram-inflight.txt")) return false;
    callbackCheckpoint = offset;
    return true;
}

void loadCallbackCheckpoint() {
    File file = LittleFS.open("/telegram-inflight.txt", "r");
    if (!file || file.size() == 0 || file.size() > 12) return;
    char value[13] = {};
    size_t size = file.size();
    if (file.readBytes(value, size) != size) return;
    int64_t offset;
    if (!parseTelegramCursor(value, offset)) return;
    callbackCheckpoint = nextUpdate = offset;
    logLine(PSTR("[Telegram] Recovering callback cursor; interrupted action will not replay"));
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
    scheduleLoaded = true;
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
    scheduleLoaded = true;
    return true;
}

// Called from the main code and from lightTimer. The timer fires whenever the
// main code yields, which analogWrite() itself may do: a nested call would
// overwrite the PWM update still in flight, so it leaves the work to the outer one.
bool defaultSchedule(Schedule& candidate) {
    DynamicJsonDocument doc(4096);
    return !deserializeJson(doc, FPSTR(DEFAULT_SCHEDULE_JSON)) &&
        parseSchedule(doc.as<JsonVariantConst>(), candidate);
}

void updateLight() {
    static bool busy = false;
    if (busy) return;
    busy = true;
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    uint32_t second = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    const bool ready = clockReady();
    for (uint8_t c = 0; c < 4; ++c) {
        uint16_t fine = ready ? interpolateFine(schedule.channels[c], second) : 0;
        if (fineLevel[c] != fine) {
            fineLevel[c] = fine;
            brightness[c] = fine / FINE_STEPS;
            analogWrite(PWM_PINS[c], gammaDuty(fine));
        }
    }
    busy = false;
}

// One conversion every 10 s without blocking: request, wait out the conversion
// time, read. A reading older than a minute is reported as a silent sensor.
void serviceWaterSensor() {
    static uint32_t timer = 0;
    static bool converting = false;
    uint32_t now = millis();
    if (converting) {
        if (now - timer < waterSensor.getConversionTime()) return;
        converting = false;
        if (waterSensor.readTemp()) {
            waterTemp = waterSensor.getTemp();
            waterTempAt = now;
            waterTempKnown = true;
        }
    } else if (!timer || now - timer >= WATER_TEMP_PERIOD_MS) {
        converting = waterSensor.requestTemp();
        timer = now ? now : 1;
    }
}

bool waterTempFresh() { return waterTempKnown && millis() - waterTempAt < WATER_TEMP_STALE_MS; }

// RTC memory outlives every reset except a power loss, so after OTA, watchdog
// or a crash the schedule resumes at once instead of waiting for NTP.
void saveClock() {
    static uint32_t last = 0;
    if (!clockReady() || millis() - last < 10000) return;
    last = millis();
    SavedClock saved = packSavedClock(uint32_t(time(nullptr)));
    ESP.rtcUserMemoryWrite(SAVED_CLOCK_RTC_OFFSET, reinterpret_cast<uint32_t*>(&saved), sizeof(saved));
}

void restoreClock() {
    // After power-on the downtime is unknown; only NTP can be trusted then.
    if (ESP.getResetInfoPtr()->reason == REASON_DEFAULT_RST) return;
    SavedClock saved{};
    uint32_t epoch = 0;
    if (!ESP.rtcUserMemoryRead(SAVED_CLOCK_RTC_OFFSET, reinterpret_cast<uint32_t*>(&saved), sizeof(saved)) ||
        !unpackSavedClock(saved, epoch)) return;
    timeval restored{time_t(epoch), 0};
    settimeofday(&restored, nullptr);
    logLine(PSTR("[Clock] Restored from RTC memory (up to ~10 s behind) until NTP answers"));
}

// Keep Bot API requests on one verified TLS connection. Reading the complete
// HTTP body (including chunk framing) is essential before reusing the socket.
class TelegramResponseBody : public Print {
public:
    static constexpr size_t LIMIT = TELEGRAM_BODY_LIMIT;
    String text;
    bool valid = true;
    explicit TelegramResponseBody(int expected) {
        if (expected > int(LIMIT)) { valid = false; text.reserve(TELEGRAM_BODY_HEAD); }
        else if (expected > 0 && !text.reserve(expected)) valid = false;
    }
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t* data, size_t size) override {
        if (valid) {
            if (telegramBodyFits(text.length(), size) &&
                text.concat(reinterpret_cast<const char*>(data), size)) return size;
            valid = false;
        }
        // Consume excess bytes without storing them; never grow without bound.
        // Only the head is kept: it names the oversized update to step over.
        if (text.length() < TELEGRAM_BODY_HEAD)
            text.concat(reinterpret_cast<const char*>(data),
                min<size_t>(size, TELEGRAM_BODY_HEAD - text.length()));
        return size;
    }
    int availableForWrite() override { return 1024; }
};

void closeTelegramConnection() {
    if (telegramConnection) {
        telegramConnection->stop();
        telegramConnection.reset();
    }
    delay(1);
}

bool api(const char* method, const String& payload, DynamicJsonDocument& response) {
    const size_t responseCapacity = response.capacity();
    response = DynamicJsonDocument(0);
    oversizedUpdate = -1;
    updateLight();
    delay(1);
    if (!telegramConnection || !telegramConnection->connected()) {
        closeTelegramConnection();
        String error;
        logLine(PSTR("[TLS] Opening Bot API connection for %s"), method);
        telegramConnection = openTelegramTls("API connect", error);
        if (!telegramConnection) {
            logLine(PSTR("[TLS] %s"), error.c_str());
            return false;
        }
    }
    HTTPClient http;
    http.useHTTP10(false);
    http.setReuse(true);
    http.setTimeout(15000);
    if (!http.begin(*telegramConnection, String(F("https://api.telegram.org/bot")) + BOT_TOKEN + "/" + method)) {
        closeTelegramConnection();
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    delay(1);
    bool ok = false;
    bool reusable = false;
    if (code > 0) {
        TelegramResponseBody body(http.getSize());
        int read = http.writeToPrint(&body);
        auto noteOversizedUpdate = [&]() {
            int64_t id;
            if (strcmp(method, "getUpdates") == 0 && parseOversizedUpdateId(body.text.c_str(), id))
                oversizedUpdate = id;
        };
        if (read >= 0 && body.valid && size_t(read) == body.text.length()) {
            response = DynamicJsonDocument(responseCapacity);
            DynamicJsonDocument filter(1536);
            fillTelegramFilter(filter, method);
            auto error = deserializeJson(response, body.text.c_str(), body.text.length(),
                DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(20));
            reusable = !error;
            ok = !error && code == 200 && response["ok"].as<bool>();
            if (!ok) {
                logLine(PSTR("[Telegram] %s failed: HTTP=%d, JSON=%s, API=%d"),
                    method, code, error.c_str(), response["error_code"] | 0);
                const char* description = response["description"] | "";
                if (*description) logLine(PSTR("[Telegram] %.140s"), description);
                // The document was allocated in full and still overflowed: the
                // update itself is too large, not the heap too fragmented.
                if (error == DeserializationError::NoMemory && response.capacity() >= responseCapacity)
                    noteOversizedUpdate();
                if (error == DeserializationError::NoMemory)
                    logLine(PSTR("[JSON] requested=%u, capacity=%u, used=%u, heap=%u, largest=%u"),
                        unsigned(responseCapacity), unsigned(response.capacity()), unsigned(response.memoryUsage()),
                        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
            }
            if (!ok && response["parameters"]["retry_after"].is<uint32_t>())
                pollInterval = min<uint32_t>(response["parameters"]["retry_after"].as<uint32_t>(), 3600) * 1000UL;
        } else {
            if (!body.valid) noteOversizedUpdate();
            logLine(PSTR("[Telegram] %s response incomplete or exceeds %u bytes; read=%d"),
                method, unsigned(TelegramResponseBody::LIMIT), read);
        }
    } else {
        logLine(PSTR("[Telegram] %s failed: transport=%d, TLS=%d"),
            method, code, telegramConnection->getLastSSLError());
    }
    if (!reusable) http.setReuse(false);
    http.end();
    if (!reusable) closeTelegramConnection();
    response.shrinkToFit();
    if (!ok) logLine(PSTR("[Heap] free=%u, largest block=%u"),
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    updateLight();
    delay(25);
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

bool sendMessage(const String& text, bool editor = false, int32_t* messageId = nullptr) {
    // URL construction for a full editor keyboard needs more working RAM than
    // a short status. Release idle TLS buffers before building that payload.
    if (editor) closeTelegramConnection();
    String payload;
    {
        DynamicJsonDocument request(editor ? 8192 : JSON_OBJECT_SIZE(2) + text.length() + 64);
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
        if (request.overflowed()) {
            logLine(PSTR("[Telegram] Cannot allocate outgoing message JSON"));
            return false;
        }
        serializeJson(request, payload);
    }
    DynamicJsonDocument response(1024);
    if (!api("sendMessage", payload, response)) {
        logLine(PSTR("[Telegram] sendMessage failed"));
        return false;
    }
    if (messageId) {
        *messageId = response["result"]["message_id"] | int32_t(0);
        if (!*messageId) return false;
    }
    return true;
}

// Best effort: Telegram refuses to delete messages older than 48 hours.
void deleteChatMessage(int32_t messageId) {
    String payload;
    {
        DynamicJsonDocument request(256);
        if (!fillStatusCommandDelete(request, TELEGRAM_OWNER_ID, messageId)) return;
        serializeJson(request, payload);
    }
    DynamicJsonDocument response(512);
    if (!api("deleteMessage", payload, response))
        logLine(PSTR("[Telegram] Could not delete message %ld"), static_cast<long>(messageId));
}

void greetOnBoot() {
    static bool attempted = false;
    static uint32_t lastAttempt = 0;
    // After an update the OTA result replaces the greeting.
    if (bootAnnounced || expectedBootMd5[0] || !telegramOnline || !pendingFirmware.fileId.isEmpty()) return;
    if (attempted && millis() - lastAttempt < 30000) return;
    attempted = true;
    lastAttempt = millis();
    updateLight();
    String text = String(F("👋 Привет! AquaLight включён и готов к управлению.\n\nДата и время: ")) +
                  formatKyivTime(time(nullptr));
    if (!fsReady) text += F("\n⚠️ LittleFS недоступна. Свет работает по графику из прошивки; сохранение графика и OTA недоступны.");
    else if (!scheduleLoaded) text += F("\n💡 График ещё не настроен. Свет выключен до сохранения графика.");
    else if (scheduleFromFirmware) text += F("\n💡 На плате не было графика — записан стартовый из прошивки. Освещение работает по нему.");
    else text += F("\n💡 График загружен. Освещение работает по расписанию.");
    text += F("\n\nОткройте редактор кнопкой ниже.\n/status — состояние контроллера\n/update — обновить прошивку");
    bootAnnounced = sendMessage(text, true);
    if (bootAnnounced) logLine(PSTR("[Telegram] Boot greeting delivered"));
}

bool otaStatus(const String& text) {
    otaState = text;
    logLine(PSTR("[OTA] %s"), text.c_str());
    if (otaMessageId) {
        String payload;
        const char* method;
        {
            DynamicJsonDocument request(1024);
            method = fillOtaStatusEdit(request, TELEGRAM_OWNER_ID, otaMessageId, otaMessageIsCaption, text);
            if (request.overflowed()) return false;
            serializeJson(request, payload);
        }
        DynamicJsonDocument response(512);
        if (api(method, payload, response)) return true;
        // Repeating a successfully applied status is harmless (e.g. after reset).
        const char* description = response["description"] | "";
        if (response["error_code"] == 400 && strstr(description, "message is not modified")) return true;
        logLine(PSTR("[OTA] Could not edit status message %ld; keeping the same message target"),
            static_cast<long>(otaMessageId));
        return false;
    }
    // Manual /update uploads are user messages and cannot be edited by the bot.
    // Create one text status only for that path, then keep editing it.
    otaMessageIsCaption = false;
    return sendMessage(text, false, &otaMessageId);
}

bool saveOtaReceipt(const char* md5) {
    if (!fsReady) return false;
    DynamicJsonDocument doc(256);
    doc["md5"] = md5;
    doc["message_id"] = otaMessageId;
    doc["caption"] = otaMessageIsCaption;
    File file = LittleFS.open("/ota-result.tmp", "w");
    if (!file) return false;
    size_t written = serializeJson(doc, file);
    file.flush(); file.close();
    return written == measureJson(doc) && LittleFS.rename("/ota-result.tmp", "/ota-result.json");
}

void loadOtaReceipt() {
    File file = LittleFS.open("/ota-result.json", "r");
    if (!file || file.size() > 256) return;
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, file)) return;
    const char* md5 = doc["md5"] | "";
    char normalized[33];
    String caption = String(F("md5:")) + md5;
    if (!parseFirmwareMd5(caption.c_str(), normalized)) return;
    memcpy(expectedBootMd5, normalized, sizeof(expectedBootMd5));
    otaMessageId = doc["message_id"] | int32_t(0);
    otaMessageIsCaption = doc["caption"] | false;
    otaState = F("Ожидается подтверждение запуска после OTA.");
}

void reportOtaBoot() {
    if (!expectedBootMd5[0]) return;
    if (bootReportAttempted && millis() - lastBootReport < 30000) return;
    bootReportAttempted = true;
    lastBootReport = millis();
    String runningMd5 = runningFirmwareMd5();
    if (runningMd5.length() != 32) {
        logLine(PSTR("[OTA] Cannot read running firmware MD5 yet; boot report will retry"));
        return;
    }
    bool matches = runningMd5.equalsIgnoreCase(expectedBootMd5);
    String text = matches ? String(F("✅ Прошивка обновлена")) :
        String(F("❌ Прошивка не обновилась: контрольная сумма отличается, работает прежняя версия"));
    text += F("\n🔄 "); text += FIRMWARE_VERSION;
    text += F(" · сборка "); text += formatShortDateTime(buildTime(), false);
    if (!fsReady) text += F("\n⚠️ LittleFS недоступна — график из прошивки");
    else if (!scheduleLoaded) text += F("\n🌑 Графика нет — свет выключен");
    else if (scheduleFromFirmware) text += F("\n💡 Записан стартовый график из прошивки");
    else text += F("\n💡 Свет работает по графику");
    text += F("\n\n/status — состояние · /update — обновление");
    // The result is a new message with a fresh editor button. Only once it has
    // been delivered is the firmware message (file and progress) removed.
    if (!sendMessage(text, true)) return;
    logLine(PSTR("[OTA] Boot result delivered: %s"), matches ? "updated" : "MD5 mismatch");
    if (otaMessageId) deleteChatMessage(otaMessageId);
    LittleFS.remove("/ota-result.json");
    expectedBootMd5[0] = '\0';
    otaMessageId = 0;
    otaMessageIsCaption = false;
    otaState = matches ? String() : String(F("Последнее обновление не установилось: MD5 отличается"));
    bootAnnounced = true;
}

void queueFirmware(JsonObjectConst message, bool fromButton = false) {
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
        String error = String(F("❌ Нужен .bin для AquaLight D1 mini с подписью md5:32_шестнадцатеричных_символа. Размер 4096…")) + String(otaCapacity()) + " байт. Отправьте файл заново.";
        if (fromButton) otaStatus(error);
        else sendMessage(error);
        return;
    }
    if (!fromButton) {
        otaMessageId = 0;
        otaMessageIsCaption = false;
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
        (source["message_id"] | int32_t(0)) > 0 &&
        parseFirmwareMd5(caption.c_str(), md5) && data == String(F("ota:")) + md5;
    bool answered;
    {
        DynamicJsonDocument request(384), response(512);
        request["callback_query_id"] = query["id"];
        request["text"] = authorized ? "Проверяю файл прошивки…" : "Кнопка недоступна.";
        String payload; serializeJson(request, payload);
        answered = api("answerCallbackQuery", payload, response);
    }
    if (!answered) {
        logLine(PSTR("[OTA] Callback not confirmed (expired or network error); no installation will start"));
        return;
    }
    delay(1);
    if (!authorized) {
        if (query["from"]["id"].as<String>() == owner)
            sendMessage(F("❌ Не удалось проверить кнопку или файл. Соберите и отправьте прошивку из VS Code заново."));
        return;
    }
    logLine(PSTR("[OTA] Install button accepted"));
    otaMessageId = source["message_id"].as<int32_t>();
    otaMessageIsCaption = true;
    if (!otaStatus(F("⏳ Запрос установки принят. Проверяю текущую версию прошивки…"))) return;
    String currentMd5 = runningFirmwareMd5();
    if (currentMd5.length() != 32) {
        otaStatus(F("❌ Не удалось прочитать текущую прошивку. Обновление не начато."));
        return;
    }
    delay(1);
    if (currentMd5.equalsIgnoreCase(md5)) {
        otaStatus(F("✅ Эта прошивка уже установлена. Повторное обновление не требуется."));
        return;
    }
    otaWindow.arm(millis());
    queueFirmware(source, true);
    otaWindow.cancel();
}

// No JSON polling document is retained during download. Only a small buffer,
// TLS buffers and the core Updater's sector buffer occupy RAM.
bool downloadFirmware(const String& path, size_t size, const char* md5, String& error) {
    // Download owns the only TLS connection. Do not retain the API socket.
    closeTelegramConnection();
    bool complete = false;
    {
        auto client = openTelegramTls("OTA connect", error);
        if (!client) return false;
        HTTPClient http;
        http.useHTTP10(true);
        http.setTimeout(15000);
        if (!http.begin(*client, String(F("https://api.telegram.org/file/bot")) + BOT_TOKEN + "/" + path)) {
            error = F("Не удалось открыть HTTPS.");
            return false;
        }
        int code = http.GET();
        if (code != HTTP_CODE_OK || http.getSize() != int(size)) {
            error = F("Ошибка HTTP или размер файла не совпадает с Telegram.");
            http.end();
            return false;
        }
        logMemory("Before Update.begin");
        if (ESP.getFreeHeap() < 10000 || ESP.getMaxFreeBlockSize() < 4608) {
            error = F("Недостаточно памяти для буфера записи OTA. Прежняя прошивка сохранена.");
            http.end();
            return false;
        }
        if (!Update.begin(size, U_FLASH)) {
            error = Update.getErrorString();
            http.end();
            return false;
        }
        logMemory("Updater ready");
        if (!Update.setMD5(md5)) {
            error = F("Неверная MD5.");
            Update.end();
            http.end();
            return false;
        }
        FirmwareMarkerScanner marker;
        OtaProgress progress;
        logLine(PSTR("[OTA] Download started: 0/%u bytes (0%%)"), unsigned(size));
        uint8_t buffer[256];
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
            if (progress.advance(received, size)) {
                logLine(PSTR("[OTA] Download: %u/%u bytes (%u%%), %lu s"),
                    unsigned(received), unsigned(size), progress.percent(received, size),
                    (millis() - started) / 1000UL);
                logMemory("OTA progress");
            }
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
    logLine(PSTR("[OTA] Download complete. Verifying MD5 and finalizing image"));
    if (!Update.end(false)) { error = Update.getErrorString(); return false; }
    // Updater's sector buffer is now free; Telegram uses only one TLS connection.
    otaStatus(F("✅ 4/5. Файл загружен на 100%, MD5 совпадает. Запись прошивки завершена."));
    return true;
}

void processFirmware() {
    if (pendingFirmware.fileId.isEmpty()) return;
    PendingFirmware firmware = pendingFirmware;
    pendingFirmware.fileId = "";
    // Do not start an invisible update if the first persistent status cannot
    // reach the owner. The short callback toast alone is insufficient.
    if (!otaStatus(F("⏳ 1/5. Запрос обновления принят платой. Подтверждаю обработку файла…"))) {
        logLine(PSTR("[OTA] Cancelled: could not deliver initial status; firmware unchanged"));
        otaState = F("Обновление отменено: не удалось отправить статус в Telegram.");
        return;
    }
    String path;
    bool acknowledged;
    {
        // Confirm consumption before reboot without consuming the next update.
        DynamicJsonDocument ack(4096);
        String request = "{\"offset\":" + String(static_cast<long long>(nextUpdate)) +
                         ",\"limit\":1,\"timeout\":0,\"allowed_updates\":[\"message\",\"callback_query\"]}";
        // An oversized next update still means Telegram accepted the offset.
        acknowledged = api("getUpdates", request, ack) || oversizedUpdate >= nextUpdate;
    }
    if (!acknowledged) {
        otaStatus(F("❌ Обновление отменено: Telegram не подтвердил обработку файла. Отправьте прошивку заново."));
        return;
    }
    otaStatus(F("📄 2/5. Получаю файл из Telegram и проверяю его размер…"));
    bool fileReady;
    {
        DynamicJsonDocument request(512), response(1024);
        request["file_id"] = firmware.fileId;
        String payload; serializeJson(request, payload);
        fileReady = api("getFile", payload, response) &&
            response["result"]["file_size"].as<size_t>() == firmware.size;
        if (fileReady) path = response["result"]["file_path"] | "";
    }
    if (!fileReady) {
        otaStatus(F("❌ Не удалось получить файл прошивки или его размер изменился. Прежняя прошивка работает. Повторите установку."));
        return;
    }
    if (path.isEmpty() || path.startsWith("/") || path.indexOf("..") >= 0 || path.indexOf(':') >= 0) {
        otaStatus(F("❌ Telegram вернул недопустимый путь файла. Обновление отменено."));
        return;
    }
    if (!saveOtaReceipt(firmware.md5)) {
        otaStatus(F("❌ Не удалось записать результат OTA в LittleFS. Обновление не начато; проверьте файловую систему."));
        return;
    }
    otaStatus(String(F("⬇️ 3/5. Скачиваю и записываю прошивку: ")) + (firmware.size / 1024) +
        F(" КБ. Ожидайте следующего этапа. Тайм-аут загрузки — 3 минуты."));
    String error;
    if (!downloadFirmware(path, firmware.size, firmware.md5, error)) {
        LittleFS.remove("/ota-result.json");
        otaStatus(String(F("❌ Обновление не установлено: ")) + error +
            F("\nРаботает прежняя прошивка. Повторите установку."));
        return;
    }
    otaStatus(F("🔄 5/5. MD5 совпадает, прошивка записана. Перезагружаюсь… После подключения подтвержу запуск новой версии."));
    delay(300);
    ESP.restart();
}

// /status is delivered over Wi-Fi after NTP sync, so "no network" and "no time"
// cannot be reported here; constants (PWM frequency) and idle states are omitted.
String controllerStatus() {
    updateLight();
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    const uint32_t second = local.tm_hour * 3600UL + local.tm_min * 60 + local.tm_sec;
    String status;
    status.reserve(640);
    status = F("🐠 AquaLight · ");
    status += formatShortDateTime(local, true);
    if (!ntpSynced) status += F(" · ⏳ без NTP");
    if (!fsReady) status += F("\n⚠️ LittleFS недоступна — график из прошивки, сохранение не работает");
    if (!scheduleLoaded) status += F("\n🌑 Графика нет — свет выключен");
    else {
        uint16_t next = 1440;
        for (const Channel& ch : schedule.channels)
            for (uint8_t i = 0; i < ch.count; ++i)
                if (uint32_t(ch.points[i].t) * 60 > second && ch.points[i].t < next) next = ch.points[i].t;
        char point[8];
        snprintf_P(point, sizeof(point), PSTR("%02u:%02u"), unsigned(next % 1440 / 60), unsigned(next % 60));
        status += F("\n🗓 По графику · следующая точка "); status += point;
        status += '\n';
        const __FlashStringHelper* icons[] = {F("⚪"), F("🔴"), F("🟢"), F("🔵")};
        for (uint8_t c = 0; c < 4; ++c) {
            const uint8_t value = brightness[c];
            const unsigned percent = (unsigned(value) * 100 + 127) / 255;
            // A lit channel never shows an empty bar or "0%".
            unsigned filled = (unsigned(value) * 10 + 127) / 255;
            if (value && !filled) filled = 1;
            status += '\n'; status += icons[c]; status += ' ';
            for (uint8_t cell = 0; cell < 10; ++cell) status += cell < filled ? F("▰") : F("▱");
            if (value && !percent) status += F(" <1%");
            else { status += ' '; status += percent; status += '%'; }
            const int8_t direction = trend(schedule.channels[c], second);
            if (direction) status += direction > 0 ? F(" ▲") : F(" ▼");
        }
    }
    if (waterTempFresh()) { status += F("\n\n🌡 Вода "); status += String(waterTemp, 1); status += F(" °C"); }
    else status += F("\n\n🌡 Датчик воды не отвечает");
    status += F("\n📶 "); status += WiFi.RSSI();
    status += F(" дБм · "); status += WiFi.localIP().toString();
    status += F("\n⏱ "); status += formatUptime();
    status += resetWasAbnormal() ? F(" · ⚠️ ") : F(" · "); status += resetReasonLabel();
    status += F("\n💾 RAM "); status += String(ESP.getFreeHeap() / 1024.0f, 1);
    status += F(" КБ (блок "); status += String(ESP.getMaxFreeBlockSize() / 1024.0f, 1);
    status += F(") · ФС ");
    FSInfo fsInfo;
    if (fsReady && LittleFS.info(fsInfo)) {
        status += fsInfo.usedBytes / 1024; status += '/';
        status += fsInfo.totalBytes / 1024; status += F(" КБ");
    } else status += F("нет");
    status += F("\n🔄 "); status += FIRMWARE_VERSION;
    status += F(" · "); status += formatShortDateTime(buildTime(), false);
    if (!otaState.isEmpty()) { status += F("\n📦 "); status += otaState; }
    else if (otaWindow.active(millis())) status += F("\n📦 Жду firmware.bin после /update");
    return status;
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
        // Delete only the owner's incoming command, after the status is delivered.
        if (sendMessage(controllerStatus()))
            deleteChatMessage(message["message_id"] | int32_t(0));
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
            pollInterval = min<uint32_t>(max<uint32_t>(pollInterval, POLL_RETRY_MS) * 2, 60000);
            return;
        }
    }
    DynamicJsonDocument request(256);
    request["offset"] = nextUpdate;
    request["limit"] = 1;
    // The first poll returns at once so the boot greeting is not held back.
    request["timeout"] = telegramOnline ? LONG_POLL_SECONDS : 0;
    JsonArray allowed = request.createNestedArray("allowed_updates");
    allowed.add("message");
    allowed.add("callback_query");
    String payload; serializeJson(request, payload);
    DynamicJsonDocument response(4096);
    if (!api("getUpdates", payload, response)) {
        if (oversizedUpdate >= nextUpdate) {
            // Unparseable forever: confirm it instead of fetching it again.
            logLine(PSTR("[Telegram] Update %ld does not fit the buffers; skipped"),
                static_cast<long>(oversizedUpdate));
            nextUpdate = oversizedUpdate + 1;
            telegramOnline = true;
            pollInterval = POLL_GAP_MS;
            return;
        }
        telegramOnline = false;
        pollInterval = min<uint32_t>(max<uint32_t>(pollInterval, POLL_RETRY_MS) * 2, 60000);
        logLine(PSTR("[Telegram] polling failed (network, TLS or API)"));
        return;
    }
    telegramOnline = true;
    // A successful poll at this offset acknowledges the interrupted callback.
    if (callbackCheckpoint && nextUpdate >= callbackCheckpoint) {
        LittleFS.remove("/telegram-inflight.txt");
        callbackCheckpoint = 0;
    }
    delay(1);
    static bool reportedReady = false;
    if (!reportedReady) {
        logLine(PSTR("[Telegram] Ready. Send /start to the bot from the owner account."));
        reportedReady = true;
    }
    pollInterval = POLL_GAP_MS;
    for (JsonObjectConst update : response["result"].as<JsonArrayConst>()) {
        int64_t id = update["update_id"].as<int64_t>();
        if (id >= nextUpdate) {
            nextUpdate = id + 1;
            if (update["callback_query"].is<JsonObjectConst>()) {
                // Write BEFORE any callback reply, hashing or further HTTPS.
                if (!saveCallbackCheckpoint(nextUpdate)) {
                    logLine(PSTR("[OTA] Cannot checkpoint callback; installation refused"));
                    continue;
                }
                delay(1);
                handleFirmwareButton(update["callback_query"].as<JsonObjectConst>());
            } else handleMessage(update["message"].as<JsonObjectConst>());
            delay(1);
        }
    }
}
void loadWifiConfig() {
    File file = LittleFS.open("/wifi.json", "r");
    if (!file || file.size() > 256) return;
    DynamicJsonDocument doc(384);
    if (deserializeJson(doc, file)) return;
    String ssid = doc["ssid"] | "", pass = doc["pass"] | "";
    if (!wifiCredentialsValid(ssid.length(), pass.length())) return;
    wifiSsid = ssid;
    wifiPass = pass;
    logLine(PSTR("[WiFi] Using the network saved from the setup page"));
}

bool saveWifiConfig(const String& ssid, const String& pass) {
    if (!fsReady) return false;
    DynamicJsonDocument doc(384);
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    File file = LittleFS.open("/wifi.tmp", "w");
    if (!file) return false;
    size_t written = serializeJson(doc, file);
    file.flush(); file.close();
    return written == measureJson(doc) && LittleFS.rename("/wifi.tmp", "/wifi.json");
}

void sendPortalPage(const __FlashStringHelper* note, bool form = true) {
    String page;
    page.reserve(1400);
    page = F("<!doctype html><html lang=ru><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>AquaLight Wi-Fi</title>"
        "<style>body{font:16px system-ui,sans-serif;background:#101b24;color:#e9f1f4;max-width:420px;margin:auto;padding:20px}"
        "input,button{font:inherit;display:block;width:100%;box-sizing:border-box;margin:6px 0 16px;padding:12px;"
        "border-radius:10px;border:1px solid #385365;background:#1b303f;color:inherit}"
        "button{background:#3ed4b5;color:#08241e;font-weight:700;border:0}.plain{background:#1b303f;color:inherit;border:1px solid #385365;font-weight:400}"
        "p{color:#a9bdc9}</style><h1>🐠 AquaLight · Wi-Fi</h1><p>");
    page += note;
    page += F("</p>");
    if (form) page += F("<form method=post action=/save><label>Сеть (SSID)<input name=ssid maxlength=32 required></label>"
        "<label>Пароль<input name=pass type=password maxlength=63></label>"
        "<button>Сохранить и перезагрузить</button></form>"
        "<form method=post action=/reset><button class=plain>Вернуть сеть из прошивки</button></form>");
    portal->send(200, F("text/html; charset=utf-8"), page);
}

void scheduleRestart() {
    restartPending = true;
    restartAt = millis() + 1500;
}

void stopPortal() {
    portal.reset();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    stationPaused = false;
    logLine(PSTR("[WiFi] Setup access point closed"));
}

void startPortal() {
    WiFi.mode(WIFI_AP_STA);
    // Open network by the owner's choice: it exists only while Wi-Fi is down and
    // can only change which network the controller joins, never read the old one.
    if (!WiFi.softAP(SETUP_AP_NAME)) {
        WiFi.mode(WIFI_STA);
        lastWifiOk = millis();
        logLine(PSTR("[WiFi] Cannot start the setup access point; retry in 3 min"));
        return;
    }
    portal.reset(new (std::nothrow) ESP8266WebServer(80));
    if (!portal) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); lastWifiOk = millis(); return; }
    portal->on("/", HTTP_GET, []() {
        sendPortalPage(F("Контроллер не может подключиться к Wi-Fi. Укажите сеть, в которой он должен работать."));
    });
    portal->on("/save", HTTP_POST, []() {
        String ssid = portal->arg("ssid"), pass = portal->arg("pass");
        if (!wifiCredentialsValid(ssid.length(), pass.length()))
            sendPortalPage(F("Нужно имя сети до 32 символов и пароль 8…63 символа (или пустой для открытой сети)."));
        else if (!saveWifiConfig(ssid, pass))
            sendPortalPage(F("Не удалось записать настройки в LittleFS."));
        else {
            sendPortalPage(F("Сохранено. Контроллер перезагружается и подключается к новой сети. "
                "Если не получится, эта точка доступа появится снова через 3 минуты."), false);
            scheduleRestart();
        }
    });
    portal->on("/reset", HTTP_POST, []() {
        LittleFS.remove("/wifi.json");
        sendPortalPage(F("Сохранённая сеть удалена. Контроллер перезагружается с сетью из прошивки."), false);
        scheduleRestart();
    });
    portal->onNotFound([]() {
        portal->sendHeader(F("Location"), F("http://192.168.4.1/"));
        portal->send(302, F("text/plain"), "");
    });
    portal->begin();
    logLine(PSTR("[WiFi] No connection for 3 min: access point %s is up, open http://192.168.4.1/"), SETUP_AP_NAME);
}

// Without Wi-Fi for WIFI_PORTAL_DELAY_MS the controller offers its own access
// point to enter new credentials; it disappears as soon as Wi-Fi is back.
void serviceWifiPortal() {
    if (restartPending) {
        if (portal) portal->handleClient();
        if (int32_t(millis() - restartAt) >= 0) ESP.restart();
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        lastWifiOk = millis();
        if (portal) stopPortal();
        return;
    }
    if (!portal) {
        if (millis() - lastWifiOk >= WIFI_PORTAL_DELAY_MS) startPortal();
        return;
    }
    portal->handleClient();
    // The radio cannot serve a phone and scan for the router at once: while
    // someone is on the setup page, stop looking for the old network.
    bool busy = WiFi.softAPgetStationNum() > 0;
    if (busy == stationPaused) return;
    stationPaused = busy;
    if (busy) { WiFi.setAutoReconnect(false); WiFi.disconnect(); }
    else { WiFi.setAutoReconnect(true); WiFi.begin(wifiSsid.c_str(), wifiPass.c_str()); }
}
} // namespace

void setup() {
    Serial.begin(115200);
    logLine(PSTR("[Boot] Build %s %s"), __DATE__, __TIME__);
    logLine(PSTR("[Reset] %s"), ESP.getResetReason().c_str());
    analogWriteRange(PWM_RANGE);
    analogWriteFreq(1000);
    for (uint8_t pin : PWM_PINS) { pinMode(pin, OUTPUT); analogWrite(pin, 0); }
    // Dark until a schedule is loaded from the file system or from the firmware.
    for (Channel& ch : schedule.channels) { ch.count = 2; ch.points[0] = {0, 0}; ch.points[1] = {1440, 0}; }
    LittleFSConfig fsConfig;
    fsConfig.setAutoFormat(false);
    LittleFS.setConfig(fsConfig);
    fsReady = LittleFS.begin();
    if (fsReady) {
        if (!loadSchedule("/schedule.json")) {
            // No computer is needed for the first start: the firmware carries the
            // starting schedule and creates the file itself.
            bool existed = LittleFS.exists("/schedule.json");
            Schedule initial;
            if (defaultSchedule(initial) && saveSchedule(initial)) {
                scheduleFromFirmware = true;
                logLine(PSTR("[FS] %s /schedule.json; starting schedule from the firmware written"),
                    existed ? "Invalid" : "No");
            } else logLine(PSTR("[FS] No valid schedule and the starting one could not be written; lights off"));
        }
        loadOtaReceipt();
        loadCallbackCheckpoint();
        loadWifiConfig();
    } else {
        // Nothing can be saved, but the aquarium should not stay dark.
        scheduleLoaded = scheduleFromFirmware = defaultSchedule(schedule);
        logLine(PSTR("[FS] Mount failed; running the starting schedule from the firmware, uploadfs required"));
    }
    if (wifiSsid.isEmpty()) { wifiSsid = F(WIFI_SSID); wifiPass = F(WIFI_PASS); }
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    settimeofday_cb(BoolCB([](bool fromSntp) { if (fromSntp) ntpSynced = true; }));
    configTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    restoreClock();
    // A timer, not loop(): the light keeps following the schedule while a long
    // poll or an OTA download blocks the main loop.
    os_timer_setfn(&lightTimer, [](void*) { updateLight(); }, nullptr);
    os_timer_arm(&lightTimer, 250, true);
    logLine(PSTR("[Heap] boot free=%u, largest block=%u\n"),
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    logLine(PSTR("[Boot] Waiting for Wi-Fi and NTP without blocking PWM"));
}

void loop() {
    trackUptime();
    logConnectivity();
    saveClock();
    serviceWaterSensor();
    serviceWifiPortal();
    if (WiFi.status() == WL_CONNECTED && clockReady()) reportOtaBoot();
    if (!pendingFirmware.fileId.isEmpty()) {
        processFirmware();
        lastPoll = millis();
    }
    if (WiFi.status() == WL_CONNECTED && clockReady() && millis() - lastPoll >= pollInterval) {
        pollBot();
        lastPoll = millis();
    }
    if (WiFi.status() == WL_CONNECTED && clockReady()) greetOnBoot();
    delay(5);
}
