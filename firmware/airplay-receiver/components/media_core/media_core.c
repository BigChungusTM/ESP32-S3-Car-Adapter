// Media-facing boundary. USB owns the bounded PCM ring and output clock.
#include "media_core.h"
#include "ipod_usb.h"
#include "iap.h"
void media_begin(void) { media_flush(); }
void media_write_pcm(const int16_t *samples, size_t frames) { ipod_usb_push_pcm(samples, frames); }
void media_set_metadata(const char *artist, const char *title, const char *album) {
    iap_set_track(artist, title, album);
}
void media_set_play_state(bool playing) { iap_set_playing(playing); }
void media_flush(void) { iap_set_playing(false); ipod_usb_flush_pcm(); }
void media_end(void) { media_flush(); }
