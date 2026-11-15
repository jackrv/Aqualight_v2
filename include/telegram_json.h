#pragma once
#include <ArduinoJson.h>
#include <string.h>

inline void fillTelegramFilter(JsonDocument& filter, const char* method) {
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
        message["caption"] = true;
        message["document"]["file_id"] = true;
        message["document"]["file_name"] = true;
        message["document"]["file_size"] = true;
        JsonObject query = item.createNestedObject("callback_query");
        query["id"] = true;
        query["data"] = true;
        query["from"]["id"] = true;
        JsonObject source = query.createNestedObject("message");
        source["chat"]["id"] = true;
        source["chat"]["type"] = true;
        source["from"]["id"] = true;
        source["document"]["file_id"] = true;
        source["document"]["file_name"] = true;
        source["document"]["file_size"] = true;
        source["caption"] = true;
    } else if (strcmp(method, "getFile") == 0) {
        filter["result"]["file_path"] = true;
        filter["result"]["file_size"] = true;
    }
}
