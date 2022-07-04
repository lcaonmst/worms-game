/* Common structs and functions.
 * Author: Kamil Zwierzchowski.
 */

#include <cstring>
#include "common.h"

#define MIN_CHAR 33
#define MAX_CHAR 126

bool check_player_name(player_name_t player_name) {
    int8 n = strlen(player_name);
    for (int8 i = 0; i < n; i++) {
        if (player_name[i] < MIN_CHAR || MAX_CHAR < player_name[i]) {
            return false;
        }
    }
    return true;
}

int32 crc32_compute(const void *buf, size_t size) {
    const int8 *p = (int8 *) buf;
    uint32_t crc;

    crc = ~0U;
    while (size--)
        crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ ~0U;
}