#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
void media_begin(void);
void media_write_pcm(const int16_t *samples, size_t frames);
void media_set_metadata(const char *artist, const char *title, const char *album);
void media_set_play_state(bool playing);
void media_flush(void);
void media_end(void);
