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
void iap_reset_protocol(void);
void iap_probe_advance(void);

// Returns true while there are queued outbound reports.
bool iap_tx_pending(void);

// Live now-playing data from the AirPlay side.
void iap_set_track(const char *artist, const char *title, const char *album);
void iap_set_playing(bool playing);
void iap_set_serial(const char *serial);

// Incoming PCM activity; does not advance the play-position clock.
void iap_note_pcm(uint32_t bytes);
// USB owner supplies cumulative successfully completed USB sample time.
void iap_note_usb_time(uint64_t total_us);

// True while an AirPlay source is actively producing audio.
bool iap_audio_active(void);

// Periodic stall watchdog: dumps handshake state if the car goes quiet
// mid-handshake. Call from the USB task loop (~10 ms tick).
void iap_watchdog(void);

// Snapshot for the status page.
typedef struct {
    char state[16];
    int cert_cur, cert_max;
    uint32_t last_latency_us;
    uint32_t tx_ack, tx_ident, tx_auth, tx_audio, tx_other;
    uint32_t seq;
    uint8_t probe_profile;
    int8_t probe_success_profile;
    uint32_t probe_attempts;
} iap_snapshot_t;

void iap_snapshot(iap_snapshot_t *out);

// Counters for the status page.
uint32_t iap_rx_packets(void);
uint32_t iap_tx_packets(void);

#ifdef __cplusplus
}
#endif
