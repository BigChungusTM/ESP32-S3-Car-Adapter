#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_transport.h"

static uint8_t received[1920];
static unsigned received_len, calls;
static uint16_t constrained_write(const void *data, uint16_t length) {
    /* Exercise backpressure, including writes that accept nothing. */
    const unsigned limits[] = {0, 800, 1, 0, 127, 1920};
    unsigned accepted = limits[calls++ % 6];
    if (accepted > length) accepted = length;
    memcpy(received + received_len, data, accepted);
    received_len += accepted;
    return accepted;
}

int main(void) {
    for (unsigned rate = 44100; rate <= 48000; rate += 3900) {
        uint32_t fraction = 0, bytes = 0;
        for (unsigned ms = 0; ms < 10000; ms++) {
            unsigned packet = audio_packet_bytes(rate, &fraction);
            assert(packet == (rate == 48000 ? 192 : 176) ||
                   (rate == 44100 && packet == 180));
            bytes += packet;
            if (ms % 10 == 9) assert(bytes == rate * 4 * (ms + 1) / 1000);
        }
        assert(fraction == 0);
    }
    uint8_t original[1920];
    for (unsigned i = 0; i < sizeof(original); i++) original[i] = i * 37;
    audio_pending_t pending = {sizeof(original), 0};
    while (pending.offset != pending.length) {
        assert(calls < 100);
        audio_pending_drain(&pending, original, constrained_write);
    }
    assert(received_len == sizeof(original));
    assert(memcmp(original, received, sizeof(original)) == 0);
    unsigned previous_calls = calls;
    audio_pending_drain(&pending, original, constrained_write);
    assert(calls == previous_calls);
    puts("Audio pacing and partial-write tests passed");
}
