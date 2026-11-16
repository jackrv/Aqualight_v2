#include <cassert>
#include <iostream>
#include "access.h"

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
    std::cout << "Extra user list: PASS\n";
}
