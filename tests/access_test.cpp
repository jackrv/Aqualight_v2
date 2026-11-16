#include <cassert>
#include <iostream>
#include "access.h"
#include "sent_messages.h"

int main() {
    char ids[MAX_EXTRA_USERS][TELEGRAM_ID_SIZE];
    assert(parseUserIds("", "1", ids, MAX_EXTRA_USERS) == 0);
    assert(parseUserIds(nullptr, "1", ids, MAX_EXTRA_USERS) == 0);
    assert(parseUserIds("222", "111", ids, MAX_EXTRA_USERS) == 1 && !strcmp(ids[0], "222"));
    assert(parseUserIds(" 222, 333;444 ", "111", ids, MAX_EXTRA_USERS) == 3);
    assert(!strcmp(ids[0], "222") && !strcmp(ids[1], "333") && !strcmp(ids[2], "444"));
    // The owner, duplicates and anything that is not a positive number are dropped.
    assert(parseUserIds("111,222,222,abc,-5,12x,0,012,,", "111", ids, MAX_EXTRA_USERS) == 1 && !strcmp(ids[0], "222"));
    // A prefix of another ID is a different account.
    assert(parseUserIds("1234,123", "12", ids, MAX_EXTRA_USERS) == 2);
    assert(parseUserIds("123456789012345678901", "1", ids, MAX_EXTRA_USERS) == 0);          // 21 digits
    assert(parseUserIds("12345678901234567890", "1", ids, MAX_EXTRA_USERS) == 1);           // 20 digits
    assert(parseUserIds("1,2,3,4,5,6", "9", ids, MAX_EXTRA_USERS) == MAX_EXTRA_USERS && !strcmp(ids[3], "4"));

    // One message of each kind per chat; the previous one is returned for deletion.
    SentMessages sent;
    assert(sentMessagesValid(sent));
    assert(rememberSent(sent, "111", KIND_STATUS, 10) == 0);
    assert(rememberSent(sent, "111", KIND_STATUS, 15) == 10);
    assert(rememberSent(sent, "111", KIND_STATUS, 15) == 0);          // the same message is never deleted
    assert(rememberSent(sent, "111", KIND_BUTTON, 16) == 0);          // kinds do not mix
    assert(rememberSent(sent, "222", KIND_STATUS, 15) == 0);          // neither do chats
    assert(rememberSent(sent, "222", KIND_STATUS, 20) == 15);
    assert(rememberSent(sent, "111", KIND_STATUS, 30) == 15);
    assert(rememberSent(sent, "111", KIND_COUNT, 31) == 0);
    assert(rememberSent(sent, "111", KIND_STATUS, 0) == 0 && rememberSent(sent, "", KIND_STATUS, 5) == 0);
    assert(rememberSent(sent, nullptr, KIND_STATUS, 5) == 0);
    assert(rememberSent(sent, "111", KIND_STATUS, 32) == 30);         // rejected calls changed nothing
    // A sixth chat reuses a slot and starts clean instead of inheriting foreign IDs.
    for (const char* chat : {"333", "444", "555"}) assert(rememberSent(sent, chat, KIND_NOTICE, 7) == 0);
    assert(rememberSent(sent, "666", KIND_STATUS, 40) == 0);
    assert(rememberSent(sent, "666", KIND_STATUS, 41) == 40);
    assert(rememberSent(sent, "111", KIND_STATUS, 50) == 0);          // "111" was the slot given away
    assert(sentMessagesValid(sent));
    SentMessages damaged = sent; damaged.magic = 0; assert(!sentMessagesValid(damaged));
    damaged = sent; memset(damaged.chats[2].chat, '9', sizeof(damaged.chats[2].chat)); assert(!sentMessagesValid(damaged));
    damaged = sent; damaged.next = SENT_CHATS; assert(!sentMessagesValid(damaged));
    std::cout << "Extra user list and sent message table: PASS\n";
}
