#pragma once
// Tiny read-only-ish FAT16 disk (8 MiB in PSRAM) backing the MSC config.
// Formatted at boot so the head unit's storage probe succeeds; the iPod
// config carries the actual audio + iAP session.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 4 MiB: fits alongside the RAOP buffer + USB ring in 8 MB PSRAM.
// (An 8 MiB disk plus those allocations exceeds PSRAM and kills USB init.)
#define MSC_DISK_SECTORS 8192u
#define MSC_DISK_SECTOR  512u

// Format the RAM disk. Identity strings go into the README so the disk
// reflects the configured adapter name / Wi-Fi credentials.
// Returns false on allocation failure.
bool msc_disk_init(const char *name, const char *ssid, const char *password);

// Raw sector access for the MSC callbacks (LBA 0..16383).
void msc_disk_read(uint32_t lba, uint8_t *dst);
void msc_disk_write(uint32_t lba, const uint8_t *src);

#ifdef __cplusplus
}
#endif
