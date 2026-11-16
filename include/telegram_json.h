#pragma once
#include <ArduinoJson.h>
#include <string.h>
#include <stdint.h>

constexpr size_t TELEGRAM_BODY_LIMIT = 8192;
inline bool telegramBodyFits(size_t stored, size_t incoming) {
    return stored <= TELEGRAM_BODY_LIMIT && incoming <= TELEGRAM_BODY_LIMIT - stored;
}

// An update that does not fit the buffers can never be parsed, and polling the
// same offset again would return it forever. Telegram always starts the body
// with the first update_id, so the head alone is enough to step over it.
constexpr size_t TELEGRAM_BODY_HEAD = 64;
inline bool parseOversizedUpdateId(const char* body, int64_t& updateId) {
    static const char prefix[] = "{\"ok\":true,\"result\":[{\"update_id\":";
    if (!body || strncmp(body, prefix, sizeof(prefix) - 1)) return false;
    const char* digits = body + sizeof(prefix) - 1;
    int64_t result = 0;
    size_t count = 0;
    for (; digits[count] >= '0' && digits[count] <= '9'; ++count) {
        if (count >= 10) return false;
        result = result * 10 + (digits[count] - '0');
    }
    // The number must be complete: a truncated head may end inside the digits.
    if (!count || digits[count] != ',' || result > INT32_MAX) return false;
    updateId = result;
    return true;
}

inline void fillTelegramFilter(JsonDocument& filter, const char* method) {
    filter["ok"] = true;
    filter["error_code"] = true;
    filter["description"] = true;
    filter["parameters"]["retry_after"] = true;
    if (strcmp(method, "getUpdates") == 0) {
        JsonObject item = filter.createNestedArray("result").createNestedObject();
        item["update_id"] = true;
        JsonObject message = item.createNestedObject("message");
        message["message_id"] = true;
        message["chat"]["id"] = true;
        message["chat"]["type"] = true;
        message["from"]["id"] = true;
        message["text"] = true;
        message["web_app_data"]["data"] = true;
        message["caption"] = true;
        message["document"]["file_id"] = true;
        message["document"]["file_name"] = true;
        message["document"]["file_size"] = true;
        JsonObject query = item.createNestedObject("callback_query");
        query["id"] = true;
        query["data"] = true;
        query["from"]["id"] = true;
        JsonObject source = query.createNestedObject("message");
        source["message_id"] = true;
        source["chat"]["id"] = true;
        source["chat"]["type"] = true;
        source["from"]["id"] = true;
        source["document"]["file_id"] = true;
        source["document"]["file_name"] = true;
        source["document"]["file_size"] = true;
        source["caption"] = true;
    } else if (strcmp(method, "sendMessage") == 0) {
        filter["result"]["message_id"] = true;
    } else if (strcmp(method, "getFile") == 0) {
        filter["result"]["file_path"] = true;
        filter["result"]["file_size"] = true;
    }
}

// A document's status lives in its caption; a standalone status uses text.
// Always address the same message and remove the install keyboard.
template <typename Text>
inline const char* fillOtaStatusEdit(JsonDocument& request, const char* owner,
                                    int32_t messageId, bool caption, const Text& text) {
    request["chat_id"] = owner;
    request["message_id"] = messageId;
    request[caption ? "caption" : "text"] = text;
    request.createNestedObject("reply_markup").createNestedArray("inline_keyboard");
    return caption ? "editMessageCaption" : "editMessageText";
}

inline bool fillStatusCommandDelete(JsonDocument& request, const char* owner, int32_t messageId) {
    if (messageId <= 0) return false;
    request["chat_id"] = owner;
    request["message_id"] = messageId;
    return true;
}
