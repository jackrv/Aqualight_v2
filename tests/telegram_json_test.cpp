#include <cassert>
#include <iostream>
#include <string>
#include "telegram_json.h"

int main() {
    DynamicJsonDocument filter(3072);
    fillTelegramFilter(filter, "getUpdates");
    assert(!filter.overflowed());
    std::string body = R"({"ok":true,"result":[{"update_id":123,"message":{"chat":{"id":123,"type":"private"},"from":{"id":123,"first_name":"ignored"},"text":")";
    body += std::string(2048, 'a');
    body += R"("}}]})";
    DynamicJsonDocument response(4096);
    auto error = deserializeJson(response, body, DeserializationOption::Filter(filter));
    assert(!error);
    assert(response["result"][0]["message"]["text"].as<std::string>().size()==2048);
    assert(!response["result"][0]["message"]["from"].containsKey("first_name"));
    response.shrinkToFit();
    assert(response.capacity()<4096);
    assert(response["result"][0]["update_id"]==123);
    body = R"({"ok":true,"result":[{"update_id":124,"callback_query":{"id":"callback","from":{"id":123},"data":"ota:0123456789abcdef0123456789abcdef","message":{"from":{"id":456},"chat":{"id":123,"type":"private"},"caption":"md5:0123456789abcdef0123456789abcdef","document":{"file_id":")";
    body += std::string(256, 'b');
    body += R"(","file_name":"firmware.bin","file_size":486768}}}}]})";
    response=DynamicJsonDocument(4096);
    error=deserializeJson(response,body,DeserializationOption::Filter(filter));
    assert(!error);
    assert(response["result"][0]["callback_query"]["message"]["document"]["file_size"]==486768);
    std::cout << "Telegram filtered maximum schedule and OTA callback fit in 4096 bytes: PASS\n";
}
