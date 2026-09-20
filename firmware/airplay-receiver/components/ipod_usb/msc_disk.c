// 4 MiB FAT16 RAM disk, formatted at boot.
//
// Geometry: 8192 sectors, 1 sector/cluster -> 8088 clusters (valid FAT16:
// needs 4085..65525), 8 reserved sectors, 2x32-sector FATs, 512 root
// entries. Data starts at LBA 104. Cluster 2 holds README.TXT.
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "msc_disk.h"

static const char *TAG = "MSCDISK";

#define SECTORS_PER_CLUSTER 1u
#define RESERVED_SECTORS    8u
#define FAT_SECTORS         32u
#define ROOT_ENTRIES        512u
#define DATA_START_LBA      (RESERVED_SECTORS + 2 * FAT_SECTORS + (ROOT_ENTRIES * 32) / MSC_DISK_SECTOR)

static uint8_t *disk;

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}

bool msc_disk_init(const char *name, const char *ssid, const char *password) {
    static bool done;
    if (done) return disk != NULL;
    done = true;
    disk = heap_caps_malloc(MSC_DISK_SECTORS * MSC_DISK_SECTOR,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!disk) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        return false;
    }
    memset(disk, 0, MSC_DISK_SECTORS * MSC_DISK_SECTOR);

    char readme_txt[512];
    snprintf(readme_txt, sizeof(readme_txt),
        "%s adapter\r\n"
        "\r\n"
        "This USB disk is only so the car stereo accepts the adapter.\r\n"
        "Music plays through the iPod interface, not from files here.\r\n"
        "\r\n"
        "Join Wi-Fi %s (password %s) and pick\r\n"
        "%s in the iPhone AirPlay menu. Status page:\r\n"
        "http://192.168.4.1/\r\n",
        name ? name : "AirPlay", ssid ? ssid : "?",
        password ? password : "?", name ? name : "AirPlay");

    // --- boot sector / BPB (LBA 0) ---
    uint8_t *b = disk;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "VOLVO   ", 8);
    put_le16(b + 11, MSC_DISK_SECTOR);
    b[13] = SECTORS_PER_CLUSTER;
    put_le16(b + 14, RESERVED_SECTORS);
    b[16] = 2;  // FATs
    put_le16(b + 17, ROOT_ENTRIES);
    put_le16(b + 19, MSC_DISK_SECTORS);
    b[21] = 0xF8;  // media
    put_le16(b + 22, FAT_SECTORS);
    put_le16(b + 24, 32);   // sectors/track
    put_le16(b + 26, 64);   // heads
    put_le32(b + 28, 0);    // hidden
    put_le32(b + 32, 0);    // large total (fits in 16-bit field)
    b[36] = 0x80;           // drive number
    b[38] = 0x29;           // extended boot signature
    put_le32(b + 39, 0xA1B2C3D4);
    memcpy(b + 43, "VOLVO APPLE", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;

    // --- FATs: media + EOF, cluster 2 = end of chain ---
    for (int f = 0; f < 2; f++) {
        uint8_t *fat = disk + (RESERVED_SECTORS + f * FAT_SECTORS) * MSC_DISK_SECTOR;
        put_le16(fat + 0, 0xFFF8);
        put_le16(fat + 2, 0xFFFF);
        put_le16(fat + 4, 0xFFFF);  // cluster 2 used by README.TXT
    }

    // --- root directory ---
    uint8_t *root = disk + (RESERVED_SECTORS + 2 * FAT_SECTORS) * MSC_DISK_SECTOR;
    memcpy(root + 0, "VOLVO APPLE", 11);
    root[11] = 0x08;  // volume label
    memcpy(root + 32, "README  TXT", 11);
    root[32 + 11] = 0x20;  // archive
    put_le16(root + 32 + 26, 2);  // first cluster
    size_t readme_len = strlen(readme_txt);
    if (readme_len > MSC_DISK_SECTOR) readme_len = MSC_DISK_SECTOR;
    put_le32(root + 32 + 28, (uint32_t) readme_len);

    // --- file data (cluster 2) ---
    memcpy(disk + DATA_START_LBA * MSC_DISK_SECTOR, readme_txt, readme_len);

    ESP_LOGI(TAG, "FAT16 ready: %u sectors, data at LBA %u",
             MSC_DISK_SECTORS, DATA_START_LBA);
    return true;
}

void msc_disk_read(uint32_t lba, uint8_t *dst) {
    if (!disk || lba >= MSC_DISK_SECTORS) {
        memset(dst, 0, MSC_DISK_SECTOR);
        return;
    }
    memcpy(dst, disk + lba * MSC_DISK_SECTOR, MSC_DISK_SECTOR);
}

void msc_disk_write(uint32_t lba, const uint8_t *src) {
    if (!disk || lba >= MSC_DISK_SECTORS) return;
    if (lba == 0) return;  // keep the boot sector pristine
    memcpy(disk + lba * MSC_DISK_SECTOR, src, MSC_DISK_SECTOR);
}
