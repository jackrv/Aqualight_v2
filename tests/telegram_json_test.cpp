#include <cassert>
#include <iostream>
#include <string>
#include "telegram_json.h"

int main() {
    assert(telegramBodyFits(0, TELEGRAM_BODY_LIMIT));
    assert(telegramBodyFits(TELEGRAM_BODY_LIMIT - 1, 1));
    assert(!telegramBodyFits(TELEGRAM_BODY_LIMIT, 1));
    assert(!telegramBodyFits(TELEGRAM_BODY_LIMIT + 1, 0));
    assert(!telegramBodyFits(0, SIZE_MAX));
    assert(!telegramBodyFits(SIZE_MAX, 1));
    DynamicJsonDocument filter(3072);
    fillTelegramFilter(filter, "getUpdates");
    assert(!filter.overflowed());
    std::string body = R"({"ok":true,"result":[{"update_id":123,"message":{"message_id":501,"chat":{"id":123,"type":"private"},"from":{"id":123,"first_name":"ignored"},"text":")";
    body += std::string(2048, 'a');
    body += R"("}}]})";
    DynamicJsonDocument response(4096);
    auto error = deserializeJson(response, body, DeserializationOption::Filter(filter));
    assert(!error);
    assert(response["result"][0]["message"]["text"].as<std::string>().size()==2048);
    assert(!response["result"][0]["message"]["from"].containsKey("first_name"));
    assert(response["result"][0]["message"]["message_id"] == 501);
    DynamicJsonDocument deletion(256);
    assert(!fillStatusCommandDelete(deletion, "123", 0));
    assert(!fillStatusCommandDelete(deletion, "123", -1));
    assert(fillStatusCommandDelete(deletion, "123", response["result"][0]["message"]["message_id"]));
    assert(deletion["chat_id"] == "123");
    assert(deletion["message_id"] == 501);
    assert(deletion.size() == 2);
    response.shrinkToFit();
    assert(response.capacity()<4096);
    assert(response["result"][0]["update_id"]==123);
    body = R"({"ok":true,"result":[{"update_id":124,"callback_query":{"id":"callback","from":{"id":123},"data":"ota:0123456789abcdef0123456789abcdef","message":{"message_id":777,"from":{"id":456},"chat":{"id":123,"type":"private"},"caption":"md5:0123456789abcdef0123456789abcdef","document":{"file_id":")";
    body += std::string(256, 'b');
    body += R"(","file_name":"firmware.bin","file_size":486768}}}}]})";
    response=DynamicJsonDocument(4096);
    error=deserializeJson(response,body,DeserializationOption::Filter(filter));
    assert(!error);
    assert(response["result"][0]["callback_query"]["message"]["document"]["file_size"]==486768);
    assert(response["result"][0]["callback_query"]["message"]["message_id"]==777);
    DynamicJsonDocument edit(1024);
    const char* method = fillOtaStatusEdit(edit, "123", 777, true, "Downloading");
    assert(std::string(method) == "editMessageCaption");
    assert(edit["message_id"] == 777);
    assert(edit["caption"] == "Downloading");
    assert(!edit.containsKey("text"));
    assert(edit["reply_markup"]["inline_keyboard"].is<JsonArray>());
    assert(edit["reply_markup"]["inline_keyboard"].size() == 0);
    edit.clear();
    method = fillOtaStatusEdit(edit, "123", 777, true, "New firmware started");
    assert(std::string(method) == "editMessageCaption" && edit["message_id"] == 777);
    edit.clear();
    method = fillOtaStatusEdit(edit, "123", 888, false, "Manual upload status");
    assert(std::string(method) == "editMessageText");
    assert(edit["text"] == "Manual upload status" && !edit.containsKey("caption"));
    // Persist the original message type across reboot, with legacy compatibility.
    DynamicJsonDocument receipt(256);
    assert(!deserializeJson(receipt, R"({"message_id":777,"caption":true})"));
    assert(receipt["caption"] | false);
    assert(!deserializeJson(receipt, R"({"message_id":888})"));
    assert(!(receipt["caption"] | false));
    filter.clear();
    fillTelegramFilter(filter, "sendMessage");
    response = DynamicJsonDocument(1024);
    error = deserializeJson(response,
        R"({"ok":true,"result":{"message_id":901,"text":"progress status","reply_markup":{}}})",
        DeserializationOption::Filter(filter));
    assert(!error);
    assert(response["result"]["message_id"] == 901);
    assert(!response["result"].containsKey("text"));
    filter.clear();
    fillTelegramFilter(filter, "editMessageText");
    error = deserializeJson(response, R"({"ok":true,"result":{"message_id":901}})",
        DeserializationOption::Filter(filter));
    assert(!error && response["ok"] == true);
    response = DynamicJsonDocument(512);
    error = deserializeJson(response,
        R"({"ok":false,"error_code":400,"description":"Bad Request: query is too old"})",
        DeserializationOption::Filter(filter));
    assert(!error);
    assert(response["error_code"] == 400);
    assert(response["description"] == "Bad Request: query is too old");
    // Oversized updates are stepped over using only the head of the body.
    int64_t skipped = 0;
    assert(parseOversizedUpdateId(R"({"ok":true,"result":[{"update_id":123456789,
"message":{"message_id":5,"from":{"id":1)", skipped) && skipped == 123456789);
    assert(parseOversizedUpdateId(R"({"ok":true,"result":[{"update_id":7,"message":{)", skipped) && skipped == 7);
    skipped = -5;
    assert(!parseOversizedUpdateId(nullptr, skipped));
    assert(!parseOversizedUpdateId("", skipped));
    assert(!parseOversizedUpdateId(R"({"ok":true,"result":[]})", skipped));
    assert(!parseOversizedUpdateId(R"({"ok":false,"error_code":409,"description":"\"update_id\":5,"})", skipped));
    assert(!parseOversizedUpdateId(R"({"ok":true,"result":[{"update_id":1234)", skipped));      // cut inside the number
    assert(!parseOversizedUpdateId(R"({"ok":true,"result":[{"update_id":,"message")", skipped));
    assert(!parseOversizedUpdateId(R"({"ok":true,"result":[{"update_id":99999999999,"m")", skipped));
    assert(!parseOversizedUpdateId(R"({"ok":true,"result":[{"update_id":-4,"message")", skipped));
    assert(skipped == -5);
    assert(TELEGRAM_BODY_HEAD >= sizeof(R"({"ok":true,"result":[{"update_id":2147483647,)"));
    std::cout << "Telegram schedule, OTA callback, status and API error filters: PASS\n";
}
