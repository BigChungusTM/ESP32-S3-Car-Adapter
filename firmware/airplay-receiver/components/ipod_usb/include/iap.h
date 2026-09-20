#pragma once
// iPod Accessory Protocol engine, ported from research/ipod (oandrew/ipod).
// Handles General + DigitalAudio + ExtendedRemote lingos; logs everything.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Normalize a raw SET_REPORT payload to link-first form. Some hosts prefix
// the wire report ID byte ([ID][link][data]); others send link-first.
// Returns bytes to skip (0 or 1). Shared with firmware so both use one rule.
static inline uint16_t iap_strip_id(const uint8_t *buf, uint16_t len) {
    if (len >= 2 && buf[0] > 0x03 && buf[1] <= 0x03) return 1;
    return 0;
}

// Feed one reassembled HID report payload (link-first, post-strip).
// Called from the TinyUSB task; TX drains from the task loop, never here.
void iap_rx_report(uint8_t report_id, const uint8_t *data, uint16_t len);

// Pump queued TX reports toward the host. Called from the TinyUSB task loop
// and the HID report-complete callback.
void iap_tx_pump(void);

// Returns true while there are queued outbound reports.
bool iap_tx_pending(void);

// Live now-playing data from the AirPlay side.
void iap_set_track(const char *artist, const char *title, const char *album);
void iap_set_playing(bool playing);
void iap_set_serial(const char *serial);

// bytes: decoded PCM bytes just pushed toward USB (position clock).
void iap_note_pcm(uint32_t bytes);

// Counters for the status page.
uint32_t iap_rx_packets(void);
uint32_t iap_tx_packets(void);

#ifdef __cplusplus
}
#endif
