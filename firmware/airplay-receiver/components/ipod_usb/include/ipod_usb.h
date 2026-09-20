#pragma once
// iPod USB device: UAC1 audio (ESP32 -> car) + HID iAP transport.
// Stage 1: enumerate, stream audio, log iAP traffic. No iAP replies yet.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the internal USB PHY + TinyUSB device stack (iPod personality:
// UAC1 audio source + HID iAP transport). Safe to call once, early in
// app_main. Returns true on success.
bool ipod_usb_init(void);
void ipod_usb_flush_pcm(void);
uint64_t ipod_usb_delivered_us(void);

// Push decoded 44.1 kHz stereo s16 PCM (from the AirPlay path) toward USB.
// Resampled to 48 kHz internally. Drops data when the host is not listening.
void ipod_usb_push_pcm(const int16_t *samples, size_t frames);

// Test tone (440 Hz left / 660 Hz right markers) instead of AirPlay PCM.
// Proves the USB audio path independently of wireless streaming.
void ipod_usb_set_tone(bool on);
bool ipod_usb_tone(void);

// Active USB audio rate in Hz (44100 default, host-switchable to 48000).
uint32_t ipod_usb_rate(void);

// Total mass-storage commands serviced (config-1 probe activity).

// Snapshot for the HTTP status page.
typedef struct {
    uint32_t boot_ms;         // task start (firmware boot reference)
    uint32_t phy_ready_ms;    // PHY + stack initialised
    uint32_t first_connect_ms;// first deliberate attach
    uint32_t mount_ms;        // first SetConfiguration (0 = never)
    uint32_t connect_attempts; // deliberate attaches so far
    bool usb_ready;        // PHY + stack initialised
    bool host_mounted;     // SetConfiguration received
    bool audio_streaming;  // host selected the audio alt setting
    bool usb_suspended;
    bool tone_on;
    uint32_t usb_rate;
    uint64_t airplay_frames_received, pcm_frames_dropped;
    uint64_t usb_iso_bytes, usb_frames_delivered;
    uint32_t pcm_frames_buffered, usb_iso_packets, usb_fifo_starved_bytes;
    uint32_t pcm_underruns;
    uint32_t iap_rx_packets;
    uint32_t iap_tx_packets;
} ipod_usb_status_t;

void ipod_usb_get_status(ipod_usb_status_t *out);

// Copy up to buf_len bytes of the recent iAP packet log (text, hex).
// Returns bytes written.
size_t ipod_usb_read_iap_log(char *buf, size_t buf_len);

// Append a formatted line to the USB log ring (visible on status page).
void ipod_usb_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
