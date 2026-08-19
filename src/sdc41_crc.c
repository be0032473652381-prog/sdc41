#include "sdc41_crc.h"

uint8_t sdc41_crc8(const uint8_t *data, size_t length) {
    uint8_t crc = 0xff;

    for (size_t byte = 0; byte < length; ++byte) {
        crc ^= data[byte];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool sdc41_crc_self_check(void) {
    static const uint8_t datasheet_example[] = {0xbe, 0xef};
    return sdc41_crc8(datasheet_example, sizeof(datasheet_example)) == 0x92u;
}
