#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// The bot keeps one message of each kind per chat: sending a new status removes
// the previous status, and so on. Every message that carries the editor button
// is one kind, so the newest button always stays in the chat.
enum MessageKind : uint8_t { KIND_BUTTON, KIND_STATUS, KIND_SCHEDULE, KIND_NOTICE, KIND_COUNT };

constexpr uint32_t SENT_MESSAGES_MAGIC = 0x53454E31;   // "SEN1"
constexpr uint8_t SENT_CHATS = 5;                      // the owner and four extra users

struct SentMessages {
    uint32_t magic = SENT_MESSAGES_MAGIC;
    struct Chat {
        char chat[21] = {};
        int32_t ids[KIND_COUNT] = {};
    } chats[SENT_CHATS];
    uint8_t next = 0;   // slot to reuse when a sixth chat appears
};

inline bool sentMessagesValid(const SentMessages& table) {
    if (table.magic != SENT_MESSAGES_MAGIC || table.next >= SENT_CHATS) return false;
    for (const SentMessages::Chat& chat : table.chats)
        if (!memchr(chat.chat, '\0', sizeof(chat.chat))) return false;
    return true;
}

// Record the newest message of a kind and return the one it replaces (0 if none).
// IDs are kept per chat: a message ID means nothing in another chat.
inline int32_t rememberSent(SentMessages& table, const char* chat, uint8_t kind, int32_t id) {
    if (!chat || !*chat || strlen(chat) >= sizeof(table.chats[0].chat) || kind >= KIND_COUNT || id <= 0) return 0;
    SentMessages::Chat* slot = nullptr;
    for (SentMessages::Chat& known : table.chats)
        if (!strcmp(known.chat, chat)) { slot = &known; break; }
    if (!slot) {
        for (SentMessages::Chat& empty : table.chats)
            if (!empty.chat[0]) { slot = &empty; break; }
        if (!slot) { slot = &table.chats[table.next]; table.next = (table.next + 1) % SENT_CHATS; }
        *slot = SentMessages::Chat();
        strcpy(slot->chat, chat);
    }
    int32_t previous = slot->ids[kind];
    slot->ids[kind] = id;
    return previous == id ? 0 : previous;
}
