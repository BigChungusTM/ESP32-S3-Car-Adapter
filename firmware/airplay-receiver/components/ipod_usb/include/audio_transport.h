#pragma once
#include <stdint.h>

// Full-speed 1 ms packets: exactly 441 frames per ten packets at 44.1 kHz.
static inline uint16_t audio_packet_bytes(uint32_t rate, uint32_t *fraction) {
    *fraction += rate;
    uint16_t frames = *fraction / 1000;
    *fraction %= 1000;
    return frames * 4;
}

typedef struct { uint16_t length, offset; } audio_pending_t;
typedef uint16_t (*audio_write_fn)(const void *, uint16_t);
static inline void audio_pending_drain(audio_pending_t *pending, const void *buffer,
                                       audio_write_fn write) {
    uint16_t left = pending->length - pending->offset;
    if (!left) return;
    uint16_t accepted = write((const uint8_t *)buffer + pending->offset, left);
    if (accepted <= left) pending->offset += accepted;
}
