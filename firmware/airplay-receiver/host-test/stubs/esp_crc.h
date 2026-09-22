#pragma once
#include <stddef.h>
#include <stdint.h>
static inline uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
    while (len--) crc = (crc * 33u) ^ *buf++;
    return crc;
}
