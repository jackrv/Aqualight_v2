#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Besides the owner, a few more Telegram accounts may use the bot. They can see
// the status and edit the schedule; firmware updates stay with the owner.
constexpr uint8_t MAX_EXTRA_USERS = 4;
constexpr size_t TELEGRAM_ID_SIZE = 21;   // up to 20 digits and the terminator

// Parse "111, 222;333" into separate IDs. Anything that is not a plain positive
// number is skipped, as are duplicates and the excluded ID (the owner).
inline uint8_t parseUserIds(const char* list, const char* excluded,
                            char ids[][TELEGRAM_ID_SIZE], uint8_t capacity) {
    uint8_t count = 0;
    if (!list) return 0;
    const char* cursor = list;
    while (*cursor) {
        while (*cursor == ',' || *cursor == ';' || *cursor == ' ') ++cursor;
        const char* start = cursor;
        bool digits = true;
        while (*cursor && *cursor != ',' && *cursor != ';' && *cursor != ' ') {
            if (*cursor < '0' || *cursor > '9') digits = false;
            ++cursor;
        }
        const size_t length = cursor - start;
        if (!length || !digits || length >= TELEGRAM_ID_SIZE || *start == '0' || count >= capacity) continue;
        if (excluded && strlen(excluded) == length && !strncmp(excluded, start, length)) continue;
        bool known = false;
        for (uint8_t i = 0; i < count; ++i)
            if (strlen(ids[i]) == length && !strncmp(ids[i], start, length)) known = true;
        if (known) continue;
        memcpy(ids[count], start, length);
        ids[count++][length] = '\0';
    }
    return count;
}
