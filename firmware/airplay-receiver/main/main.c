#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "raop.h"
#include "log_util.h"
#include "ipod_usb.h"
#include "iap.h"

static const char *TAG = "AIRPLAY";
log_level raop_loglevel = lINFO, util_loglevel = lWARN;
static uint8_t *audio_buffer;
static const size_t audio_buffer_size = 768 * 1024;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t pcm_bytes, pcm_nonzero;
static uint32_t pcm_callbacks, sessions;
static char title[192], artist[192], album[192];

u32_t _gettime_ms_(void) { return (u32_t)(esp_timer_get_time() / 1000); }
const char *logtime(void) { return "RAOP"; }
void logprint(const char *format, ...) {
    va_list args; va_start(args, format); vprintf(format, args); va_end(args);
}

static void audio_data(const uint8_t *data, size_t length, uint32_t playtime) {
    (void)playtime;
    // Decoded PCM is 44.1 kHz stereo s16; forward complete frames to USB.
    ipod_usb_push_pcm((const int16_t *) data, length / 4);
    uint32_t nonzero = 0;
    for (size_t i = 0; i < length; i++) nonzero += data[i] != 0;
    portENTER_CRITICAL(&stats_lock);
    pcm_bytes += length; pcm_nonzero += nonzero; pcm_callbacks++;
    portEXIT_CRITICAL(&stats_lock);
    // First milestone: consume and measure decoded PCM. No car/DAC output yet.
}

static bool audio_command(raop_event_t event, ...) {
    va_list args; va_start(args, event);
    switch (event) {
    case RAOP_SETUP: {
        uint8_t **buffer = va_arg(args, uint8_t **);
        size_t *size = va_arg(args, size_t *);
        *buffer = audio_buffer; *size = audio_buffer_size;
        portENTER_CRITICAL(&stats_lock); sessions++; portEXIT_CRITICAL(&stats_lock);
        ESP_LOGI(TAG, "Session setup");
        break;
    }
    case RAOP_METADATA: {
        const char *a = va_arg(args, char *);
        const char *al = va_arg(args, char *);
        const char *t = va_arg(args, char *);
        portENTER_CRITICAL(&stats_lock);
        snprintf(artist, sizeof(artist), "%s", a ? a : "");
        snprintf(album, sizeof(album), "%s", al ? al : "");
        snprintf(title, sizeof(title), "%s", t ? t : "");
        portEXIT_CRITICAL(&stats_lock);
        iap_set_track(a, t, al);
        ESP_LOGI(TAG, "Metadata received");
        break;
    }
    case RAOP_STREAM: iap_set_playing(true); ESP_LOGI(TAG, "Stream started"); break;
    case RAOP_PLAY: iap_set_playing(true); ESP_LOGI(TAG, "Playback scheduled"); break;
    case RAOP_STOP: iap_set_playing(false); ESP_LOGI(TAG, "Stream stopped"); break;
    case RAOP_FLUSH: iap_set_playing(false); ESP_LOGI(TAG, "Stream flushed"); break;
    default: break;
    }
    va_end(args);
    return true;
}

static esp_err_t usb_handler(httpd_req_t *request) {
    char query[32] = {0};
    httpd_req_get_url_query_str(request, query, sizeof(query));
    char val[8] = {0};
    if (httpd_query_key_value(query, "profile", val, sizeof(val)) == ESP_OK) {
        usb_profile_t p = ipod_usb_profile();
        if (!strcmp(val, "both")) p = USB_PROFILE_BOTH;
        else if (!strcmp(val, "msc")) p = USB_PROFILE_MSC;
        else if (!strcmp(val, "ipod")) p = USB_PROFILE_IPOD;
        else {
            httpd_resp_set_type(request, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(request, "use ?profile=both|msc|ipod\n");
        }
        ipod_usb_set_profile(p);
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        httpd_resp_sendstr(request, "profile saved, rebooting\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, "use ?profile=both|msc|ipod\n");
}

static esp_err_t reboot_handler(httpd_req_t *request) {
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_sendstr(request, "rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t tone_handler(httpd_req_t *request) {
    char query[32] = {0};
    httpd_req_get_url_query_str(request, query, sizeof(query));
    char val[8] = {0};
    if (httpd_query_key_value(query, "on", val, sizeof(val)) == ESP_OK)
        ipod_usb_set_tone(val[0] == '1');
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, ipod_usb_tone() ? "tone on\n" : "tone off\n");
}

static esp_err_t status_handler(httpd_req_t *request) {
    // NOTE: heap, not stack: the default httpd worker stack is 4 KB and a
    // multi-KB stack buffer here overflowed it (connection resets).
    static const size_t RESPONSE_SZ = 4096;
    char *response = heap_caps_malloc(RESPONSE_SZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    char t[192], a[192];
    if (!response) return ESP_ERR_NO_MEM;
    uint64_t bytes, nonzero; uint32_t callbacks, count;
    portENTER_CRITICAL(&stats_lock);
    bytes = pcm_bytes; nonzero = pcm_nonzero; callbacks = pcm_callbacks; count = sessions;
    memcpy(t, title, sizeof(t)); memcpy(a, artist, sizeof(a));
    portEXIT_CRITICAL(&stats_lock);
    ipod_usb_status_t usb;
    ipod_usb_get_status(&usb);
    int n = snprintf(response, RESPONSE_SZ,
        "%s — receiver test\n\n"
        "Select this device in your iPhone's AirPlay output menu.\n\n"
        "Sessions: %lu\nPCM bytes: %llu\nNonzero PCM bytes: %llu\n"
        "PCM callbacks: %lu\nArtist: %s\nTitle: %s\nPSRAM bytes: %u\n\n"
        "USB iPod: profile=%d ready=%d mounted=%d audio=%d suspended=%d rate=%lu tone=%d underruns=%lu msc=%lu iAP rx=%lu tx=%lu\n\n"
        "USB log:\n",
        CONFIG_ADAPTER_NAME,
        (unsigned long)count, (unsigned long long)bytes, (unsigned long long)nonzero,
        (unsigned long)callbacks, a, t, (unsigned)esp_psram_get_size(),
        (int)usb.profile,
        usb.usb_ready, usb.host_mounted, usb.audio_streaming, usb.usb_suspended,
        (unsigned long)usb.usb_rate, usb.tone_on,
        (unsigned long)usb.pcm_underruns, (unsigned long)usb.msc_ops,
        (unsigned long)usb.iap_rx_packets, (unsigned long)usb.iap_tx_packets);
    if (n > 0 && (size_t)n < RESPONSE_SZ)
        ipod_usb_read_iap_log(response + n, RESPONSE_SZ - (size_t)n);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, response);
    heap_caps_free(response);
    return err;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) ESP_LOGI(TAG, "Wi-Fi client joined");
    if (id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "Wi-Fi client left");
}

void app_main(void) {
    ESP_LOGI(TAG, "PSRAM detected: %u bytes", (unsigned)esp_psram_get_size());
    ESP_ERROR_CHECK(nvs_flash_init());
    // USB first after NVS (profile lives there): the car/host enumerates us
    // as soon as we're plugged in.
    // Note: enabling the OTG peripheral takes over GPIO19/20, so the
    // USB-Serial/JTAG console goes quiet from here on; use the HTTP status page.
    ipod_usb_init();
    audio_buffer = heap_caps_malloc(audio_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(audio_buffer);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    assert(netif);
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, CONFIG_ADAPTER_HOSTNAME));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    wifi_config_t wifi = {0};
    snprintf((char *) wifi.ap.ssid, sizeof(wifi.ap.ssid), "%s", CONFIG_ADAPTER_SSID);
    wifi.ap.ssid_len = (uint8_t) strlen(CONFIG_ADAPTER_SSID);
    snprintf((char *) wifi.ap.password, sizeof(wifi.ap.password), "%s", CONFIG_ADAPTER_PASSWORD);
    wifi.ap.channel = 6;
    wifi.ap.max_connection = 3;
    wifi.ap.authmode = strlen(CONFIG_ADAPTER_PASSWORD) >= 8 ?
        WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_ADAPTER_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(CONFIG_ADAPTER_NAME));
    esp_netif_ip_info_t info; uint8_t mac[6];
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &info));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    assert(raop_create(info.ip.addr, CONFIG_ADAPTER_NAME, mac, 0, audio_command, audio_data));
    httpd_handle_t server = NULL; httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    httpd_uri_t status = {.uri = "/", .method = HTTP_GET, .handler = status_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    httpd_uri_t tone = {.uri = "/tone", .method = HTTP_GET, .handler = tone_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &tone));
    httpd_uri_t usb = {.uri = "/usb", .method = HTTP_GET, .handler = usb_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &usb));
    httpd_uri_t reboot = {.uri = "/reboot", .method = HTTP_GET, .handler = reboot_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reboot));
    ESP_LOGI(TAG, "READY ssid=%s ip=" IPSTR " AirPlay=%s", CONFIG_ADAPTER_SSID, IP2STR(&info.ip), CONFIG_ADAPTER_NAME);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint64_t bytes, nonzero; uint32_t callbacks;
        portENTER_CRITICAL(&stats_lock);
        bytes = pcm_bytes; nonzero = pcm_nonzero; callbacks = pcm_callbacks;
        portEXIT_CRITICAL(&stats_lock);
        ESP_LOGI(TAG, "PCM bytes=%llu nonzero=%llu callbacks=%lu free_internal=%u",
                 (unsigned long long)bytes, (unsigned long long)nonzero,
                 (unsigned long)callbacks, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
