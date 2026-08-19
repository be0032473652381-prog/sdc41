#ifndef SDC41_CRC_H
#define SDC41_CRC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t sdc41_crc8(const uint8_t *data, size_t length);
bool sdc41_crc_self_check(void);

#endif
