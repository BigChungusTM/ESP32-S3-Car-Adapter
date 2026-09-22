#pragma once
#include <stdint.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define NVS_READONLY 0
#define NVS_READWRITE 1
static inline esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    (void)name; (void)mode; (void)h; return ESP_FAIL;
}
static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) {
    (void)h; (void)key; (void)v; return ESP_FAIL;
}
static inline esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) {
    (void)h; (void)key; (void)v; return ESP_FAIL;
}
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_FAIL; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
