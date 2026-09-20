// iPod USB device for the Volvo head unit.
//
// Single configuration mirroring the captured iPod layout:
//   if0  Audio Control (mic input terminal -> USB streaming output terminal)
//   if1  Audio Streaming, alt1: 48 kHz stereo s16 device->host, iso IN EP 0x81
//   if2  HID iAP transport (208-byte report desc), interrupt IN EP 0x83,
//        host->device via control SET_REPORT.
//
// Audio: AirPlay PCM (44.1 kHz) is resampled to 48 kHz and paced at 192 B/ms.
// iAP stage 1: log traffic, no replies yet.
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"

#include "tusb.h"
#include "class/audio/audio.h"

#include "ipod_usb.h"
#include "iap.h"

static const char *TAG = "IPODUSB";

static void iap_logf(const char *fmt, ...);

//--------------------------------------------------------------------+
// USB identity (period-correct iPod Classic)
//--------------------------------------------------------------------+

#define USB_VID_APPLE       0x05AC
#define USB_PID_IPOD        0x1261
#define USB_BCD_DEVICE      0x0100

#define ITF_NUM_AUDIO_CTRL  0
#define ITF_NUM_AUDIO_STREAM 1
#define ITF_NUM_HID         2
#define ITF_COUNT           3

#define EP_ADDR_AUDIO_IN    0x81
#define EP_ADDR_HID_IN      0x83

#define UAC1_BCD_ADC        0x0100
#define AC_TOTAL_LEN        (9 + 9 + 12 + 9) // AC interface + header + input + output terminals

//--------------------------------------------------------------------+
// HID report descriptor (captured from a real iPod, 208 bytes)
//--------------------------------------------------------------------+

static const uint8_t hid_report_desc[] = {
#include "hid_report_desc.inc"
};

//--------------------------------------------------------------------+
// Descriptors
//--------------------------------------------------------------------+

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID_APPLE,
    .idProduct = USB_PID_IPOD,
    .bcdDevice = USB_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// Single configuration: the iPod personality (UAC1 + HID iAP).
// Served for descriptor index 0 AND 1 and selected by value 1 OR 2, so both
// index-driven and value-hardcoded hosts reach it.
static const uint8_t desc_config[] = {
    // Config: 3 interfaces, "iPod USB Interface", self-powered 500 mA.
    // Bit 7 is mandatory per USB spec (the TUD_CONFIG_DESCRIPTOR helper ORs it).
    9, TUSB_DESC_CONFIGURATION, U16_TO_U8S_LE(9 + AC_TOTAL_LEN + 9 + (9 + 7 + 14 + 9 + 7) + (9 + 9 + 7)),
    ITF_COUNT, 2, 4, (TUSB_DESC_CONFIG_ATT_SELF_POWERED | 0x80), 250,

    // Interface 0: Audio Control (UAC1, protocol 0)
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_CTRL, 0, 0, TUSB_CLASS_AUDIO,
    AUDIO_SUBCLASS_CONTROL, 0, 0,
    // AC header, UAC1, total 30, 1 streaming interface (if1)
    9, TUSB_DESC_CS_INTERFACE, AUDIO10_CS_AC_INTERFACE_HEADER, U16_TO_U8S_LE(UAC1_BCD_ADC),
    U16_TO_U8S_LE(AC_TOTAL_LEN), 1, ITF_NUM_AUDIO_STREAM,
    // Input terminal: microphone, stereo L+R
    12, TUSB_DESC_CS_INTERFACE, AUDIO10_CS_AC_INTERFACE_INPUT_TERMINAL,
    1, U16_TO_U8S_LE(AUDIO_TERM_TYPE_IN_GENERIC_MIC), 0, 2,
    U16_TO_U8S_LE(0x0003), 0, 0,
    // Output terminal: USB streaming, sourced from terminal 1
    9, TUSB_DESC_CS_INTERFACE, AUDIO10_CS_AC_INTERFACE_OUTPUT_TERMINAL,
    2, U16_TO_U8S_LE(AUDIO_TERM_TYPE_USB_STREAMING), 0, 1, 0,

    // Interface 1 alt 0: zero bandwidth
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_STREAM, 0, 0, TUSB_CLASS_AUDIO,
    AUDIO_SUBCLASS_STREAMING, 0, 0,
    // Interface 1 alt 1: operational
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_STREAM, 1, 1, TUSB_CLASS_AUDIO,
    AUDIO_SUBCLASS_STREAMING, 0, 0,
    // AS general: terminal link 2, PCM
    7, TUSB_DESC_CS_INTERFACE, AUDIO10_CS_AS_INTERFACE_AS_GENERAL,
    2, 1, U16_TO_U8S_LE(AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    // Format type I: stereo s16, discrete rates 44.1 kHz then 48 kHz.
    // 44100 = 0xAC44, 48000 = 0xBB80 (LE, 3 bytes each). 44100 is the
    // default the reference stack negotiates; both are genuinely implemented.
    14, TUSB_DESC_CS_INTERFACE, AUDIO10_CS_AS_INTERFACE_FORMAT_TYPE,
    AUDIO10_FORMAT_TYPE_I, IPOD_USB_CHANNELS, IPOD_USB_BYTES_PER_SAMPLE, 16,
    2, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00,
    // Iso IN endpoint, 192 B, 1 ms
    9, TUSB_DESC_ENDPOINT, EP_ADDR_AUDIO_IN, TUSB_XFER_ISOCHRONOUS,
    U16_TO_U8S_LE(IPOD_USB_EP_IN_SIZE), 1, 0, 0,
    // EP general: sampling-frequency control present
    7, TUSB_DESC_CS_ENDPOINT, AUDIO10_CS_EP_SUBTYPE_GENERAL, 0x01, 0, U16_TO_U8S_LE(0),

    // Interface 2: HID iAP transport (0x21 = HID desc, 0x22 = report desc)
    9, TUSB_DESC_INTERFACE, ITF_NUM_HID, 0, 1, TUSB_CLASS_HID, 0, 0, 0,
    9, 0x21, U16_TO_U8S_LE(0x0111), 0, 1, 0x22,
    U16_TO_U8S_LE(sizeof(hid_report_desc)),
    7, TUSB_DESC_ENDPOINT, EP_ADDR_HID_IN, TUSB_XFER_INTERRUPT,
    U16_TO_U8S_LE(64), 1,
};

static char serial_str[32] = "VOLVOAIRPLAY1";

static const char *string_desc_arr[] = {
    (const char[]) {0x09, 0x04},  // 0: English (0x0409)
    "Apple Inc.",                 // 1: manufacturer
    "iPod",                       // 2: product
    serial_str,                   // 3: serial
    "iPod USB Interface",         // 4: config
};

static uint16_t _desc_str[64];

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *) &desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    if (index == 0 || index == 1) return desc_config;
    iap_logf("config desc idx %u??", index);
    return NULL;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    uint8_t count = (uint8_t) (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]));
    if (index >= count) return NULL;
    const char *str = string_desc_arr[index];
    // First entry is the langid list (already UTF-16LE bytes)
    if (index == 0) {
        memcpy(&_desc_str[1], str, 2);
        _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 + 2);
        return _desc_str;
    }
    uint8_t len = (uint8_t) strlen(str);
    if (len > 62) len = 62;
    for (uint8_t i = 0; i < len; i++) _desc_str[1 + i] = str[i];
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * len + 2);
    return _desc_str;
}

//--------------------------------------------------------------------+
// State + iAP log ring
//--------------------------------------------------------------------+

static bool usb_ready;
static bool host_mounted;
static bool ever_mounted;
static int64_t boot_us;
static int64_t phy_ready_us;
static int64_t first_connect_us;
static int64_t mount_us;
static uint8_t connect_attempts;
static bool audio_streaming;
static uint32_t pcm_underruns;

#define IAP_LOG_LINES 48
#define IAP_LOG_LINE  96
static char iap_log[IAP_LOG_LINES][IAP_LOG_LINE];
static unsigned iap_log_head;
static portMUX_TYPE iap_log_lock = portMUX_INITIALIZER_UNLOCKED;

static void iap_logf(const char *fmt, ...) {
    char line[IAP_LOG_LINE];
    int prefix = snprintf(line, sizeof(line), "[%lu] ",
                           (unsigned long) (esp_timer_get_time() / 1000));
    if (prefix < 0) prefix = 0;
    if ((size_t) prefix >= sizeof(line)) prefix = (int) sizeof(line) - 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + prefix, sizeof(line) - (size_t) prefix, fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&iap_log_lock);
    strncpy(iap_log[iap_log_head % IAP_LOG_LINES], line, IAP_LOG_LINE - 1);
    iap_log[iap_log_head % IAP_LOG_LINES][IAP_LOG_LINE - 1] = 0;
    iap_log_head++;
    portEXIT_CRITICAL(&iap_log_lock);
}

void tud_mount_cb(void) {
    host_mounted = true;
    ever_mounted = true;
    if (mount_us == 0) {
        mount_us = esp_timer_get_time();
        iap_logf("first mount at %lums", (unsigned long) (mount_us / 1000));
    }
    iap_logf("USB mounted (SetConfiguration)");
    ESP_LOGI(TAG, "host mounted (SetConfiguration)");
}

void tud_umount_cb(void) {
    host_mounted = false;
    audio_streaming = false;
    iap_logf("USB unmounted");
    ESP_LOGI(TAG, "host unmounted");
}

//--------------------------------------------------------------------+
// Audio class callbacks (UAC1)
//--------------------------------------------------------------------+

// Track the streaming alt setting so the pump only feeds an open stream.
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    uint8_t itf = TU_U16_LOW(p_request->wIndex);
    uint8_t alt = TU_U16_LOW(p_request->wValue);
    if (itf == ITF_NUM_AUDIO_STREAM) {
        audio_streaming = (alt == 1);
        iap_logf("audio alt=%u streaming=%d", alt, audio_streaming);
        ESP_LOGI(TAG, "audio alt=%u streaming=%d", alt, audio_streaming);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    (void) p_request;
    return true;
}

static uint32_t usb_rate = 44100;  // active rate; default matches reference stack
static bool usb_suspended;

// Endpoint requests: accept either advertised rate and switch the pump.
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void) rhport;
    if (p_request->bRequest == AUDIO_CS_REQ_CUR &&
        TU_U16_HIGH(p_request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        p_request->wLength == 3) {
        uint32_t freq = ((uint32_t) pBuff[2] << 16) | ((uint32_t) pBuff[1] << 8) | pBuff[0];
        if (freq == 44100 || freq == 48000) {
            usb_rate = freq;
            iap_logf("rate %lu Hz", (unsigned long) freq);
            ESP_LOGI(TAG, "host selected %lu Hz", (unsigned long) freq);
            return true;
        }
        iap_logf("reject rate %lu", (unsigned long) freq);
        ESP_LOGW(TAG, "reject sample rate %lu", (unsigned long) freq);
    }
    return false;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
    usb_suspended = true;
    iap_logf("USB suspended");
    ESP_LOGI(TAG, "USB suspended");
}

void tud_resume_cb(void) {
    usb_suspended = false;
    iap_logf("USB resumed");
    ESP_LOGI(TAG, "USB resumed");
}

uint32_t ipod_usb_rate(void) { return usb_rate; }

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void) rhport; (void) p_request; (void) pBuff;
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void) rhport; (void) p_request; (void) pBuff;
    return false;  // no feature unit in our topology
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport; (void) p_request;
    return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport; (void) p_request;
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport; (void) p_request;
    return false;
}

//--------------------------------------------------------------------+
// HID iAP transport callbacks (stage 1: log host->device, send nothing)
//--------------------------------------------------------------------+

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void) instance;
    return hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void) instance; (void) report_id; (void) report_type;
    // Mirror ipod-gadget: answer with zeros, never stall. Some head units
    // poll GET_REPORT during the handshake and wedge on a stall.
    if (reqlen > 64) reqlen = 64;
    memset(buffer, 0, reqlen);
    return reqlen;
}

// Apple's vendor request 0x40 (mirrors ipod-gadget: ACK empty).
// All vendor-type control transfers land here regardless of interface.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage == CONTROL_STAGE_SETUP && request->bRequest == 0x40) {
        iap_logf("RX vendor 0x40 type=%02X val=%04X idx=%04X len=%u",
                 request->bmRequestType, request->wValue, request->wIndex,
                 request->wLength);
        ESP_LOGI(TAG, "Apple vendor 0x40 ACK");
        return tud_control_status(rhport, request);
    }
    return false;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void) instance;
    (void) report_type;
    // Wire format varies: some hosts include the report ID byte ([ID][link]),
    // some send link-first. One shared rule (see iap_strip_id).
    uint16_t skip = iap_strip_id(buffer, bufsize);
    uint8_t wire_id = skip ? buffer[0] : report_id;
    const uint8_t *data = buffer + skip;
    uint16_t len = bufsize - skip;
    unsigned n = len > 24 ? 24 : len;
    char hex[24 * 3 + 1];
    for (unsigned i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    hex[3 * n] = 0;
    iap_logf("RX wire=%u len=%u %s%s", wire_id, len, hex, len > 24 ? "..." : "");
    iap_rx_report(wire_id, data, len);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void) instance; (void) report; (void) len;
    iap_tx_pump();
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {
    (void) instance;
    ESP_LOGI(TAG, "HID protocol=%u", protocol);
}


// PCM ring + 44.1 kHz -> 48 kHz linear resampler
//--------------------------------------------------------------------+
// AirPlay delivers 44100 Hz stereo s16. USB advertises 48000 Hz only, so the
// pump resamples in exact 10 ms batches: 441 in -> 480 out.

#define RING_FRAMES   (441 * 12)   // ~120 ms @44.1k stereo s16 in PSRAM
static int16_t *ring;
static uint32_t ring_wr;           // total frames ever written
static uint32_t ring_rd;           // total frames ever consumed
static float resample_pos;         // fractional input position carried across batches
static bool tone_on;               // stereo marker tone instead of AirPlay PCM
static float tone_phase_l, tone_phase_r;

void ipod_usb_set_tone(bool on) {
    tone_on = on;
    ESP_LOGI(TAG, "tone %s", on ? "on (440 L / 660 R)" : "off");
}

bool ipod_usb_tone(void) { return tone_on; }

void ipod_usb_push_pcm(const int16_t *samples, size_t frames) {
    if (!ring) return;
    iap_note_pcm((uint32_t) (frames * 4));
    uint32_t avail = ring_wr - ring_rd;
    if (avail >= RING_FRAMES) return;  // full: drop (host not keeping up)
    uint32_t space = RING_FRAMES - avail;
    if (frames > space) frames = space;
    for (size_t i = 0; i < frames; i++) {
        uint32_t idx = (ring_wr + i) % RING_FRAMES;
        ring[2 * idx] = samples[2 * i];
        ring[2 * idx + 1] = samples[2 * i + 1];
    }
    ring_wr += (uint32_t) frames;
}

// Produce one 10 ms batch at the active rate: 441 frames passthrough at
// 44.1 kHz, or 441 in -> 480 out linear resample at 48 kHz. Both exact.
static void produce_batch(int16_t *out, uint32_t rate) {
    if (tone_on) {
        for (uint32_t i = 0; i < rate / 100; i++) {
            tone_phase_l += 440.0f / (float) rate;
            if (tone_phase_l >= 1.0f) tone_phase_l -= 1.0f;
            tone_phase_r += 660.0f / (float) rate;
            if (tone_phase_r >= 1.0f) tone_phase_r -= 1.0f;
            out[2 * i] = (int16_t) (3276.0f * sinf(6.2831853f * tone_phase_l));
            out[2 * i + 1] = (int16_t) (3276.0f * sinf(6.2831853f * tone_phase_r));
        }
        return;
    }
    if (rate == 44100) {
        uint32_t avail = ring_wr - ring_rd;
        if (avail < 441) {
            memset(out, 0, 441 * 2 * sizeof(int16_t));
            // Count starvation only while a source is actually playing;
            // an open stream with no AirPlay session is expected silence.
            if (tone_on || iap_audio_active()) pcm_underruns++;
            ring_rd = ring_wr;
            return;
        }
        for (uint32_t i = 0; i < 441; i++) {
            uint32_t idx = (ring_rd + i) % RING_FRAMES;
            out[2 * i] = ring[2 * idx];
            out[2 * i + 1] = ring[2 * idx + 1];
        }
        ring_rd += 441;
        return;
    }
    // 48000: linear resample, 441 in -> 480 out.
    const float step = 44100.0f / 48000.0f;
    uint32_t avail = ring_wr - ring_rd;
    if (avail < 443) {  // need 441 + lookahead; else silence
        memset(out, 0, 480 * 2 * sizeof(int16_t));
        if (tone_on || iap_audio_active()) pcm_underruns++;
        ring_rd = ring_wr;  // resync: drop stale backlog
        resample_pos = 0;
        return;
    }
    for (int i = 0; i < 480; i++) {
        float p = resample_pos + i * step;
        uint32_t j = (uint32_t) p;
        float f = p - (float) j;
        uint32_t a = (ring_rd + j) % RING_FRAMES;
        uint32_t b = (ring_rd + j + 1) % RING_FRAMES;
        for (int ch = 0; ch < 2; ch++) {
            float s = (float) ring[2 * a + ch] * (1.0f - f) + (float) ring[2 * b + ch] * f;
            out[2 * i + ch] = (int16_t) s;
        }
    }
    float consumed = 480.0f * step;          // 441.0 exactly
    uint32_t whole = (uint32_t) (resample_pos + consumed);
    resample_pos = resample_pos + consumed - (float) whole;
    ring_rd += whole;
}

static int16_t stage_buf[480 * 2];

static void audio_pump(void) {
    if (!tud_mounted() || !audio_streaming || usb_suspended) return;
    uint32_t rate = usb_rate;
    produce_batch(stage_buf, rate);
    uint16_t done = tud_audio_write(stage_buf, (uint16_t) ((rate / 100) * 4));
    (void) done;  // FIFO absorbs; leftovers re-created next batch
}

//--------------------------------------------------------------------+
// USB task + init
//--------------------------------------------------------------------+

// Attach strategy: hold disconnected across boot, attach deliberately once
// firmware is ready, then re-attach on a short schedule until the first
// mount. Car hosts that scan once miss an immediately-present device;
// a delayed fresh insertion is re-enumerated reliably.
static int64_t next_attach_us;

// Retry offsets after the first deliberate attach (ms).
static const uint32_t retry_schedule_ms[] = {3000, 3000, 4000, 5000, 15000, 15000, 15000};
static uint8_t retry_idx;

static bool stack_ready;

static void usb_stack_start(void) {
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };
    usb_phy_handle_t phy = NULL;
    if (usb_new_phy(&phy_config, &phy) != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy failed");
        return;
    }
    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
    tusb_init(0, &dev_init);
    phy_ready_us = esp_timer_get_time();
    stack_ready = true;
}

static void do_attach(const char *why) {
    connect_attempts++;
    if (first_connect_us == 0) first_connect_us = esp_timer_get_time();
    iap_logf("USB attach #%u (%s)", connect_attempts, why);
    ESP_LOGI(TAG, "USB attach #%u (%s)", connect_attempts, why);
    if (stack_ready) tud_connect();
}

static void attach_poll(void) {
    if (ever_mounted) return;
    if (tud_mounted() || retry_idx >= sizeof(retry_schedule_ms) / sizeof(retry_schedule_ms[0]))
        return;
    int64_t now = esp_timer_get_time();
    if (now < next_attach_us) return;
    iap_logf("USB re-attach (unmounted)");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(400));
    do_attach("retry");
    next_attach_us = esp_timer_get_time() +
        (int64_t) retry_schedule_ms[retry_idx++] * 1000;
}

static void tusb_task(void *arg) {
    (void) arg;
    boot_us = esp_timer_get_time();
    // Hold electrically quiet across boot: the stack (and its pullup) only
    // starts after the configured delay, so the host sees one clean insertion
    // instead of a half-boot device. tud_task() is never called before init,
    // which also avoids it blocking forever with no SOF events arriving.
    while (esp_timer_get_time() - boot_us <
           (int64_t) CONFIG_IPOD_USB_ATTACH_DELAY_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    usb_stack_start();
    // Stack init attaches immediately: this IS the first deliberate attach.
    connect_attempts++;
    first_connect_us = esp_timer_get_time();
    iap_logf("USB attach #1 (first)");
    ESP_LOGI(TAG, "USB attach #1 (first)");
    retry_idx = 1;  // schedule[0] consumed by the first attach timing
    next_attach_us = first_connect_us +
        (int64_t) retry_schedule_ms[0] * 1000;
    TickType_t last = xTaskGetTickCount();
    while (true) {
        if (!stack_ready) {
            vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
            continue;
        }
        tud_task();
        audio_pump();
        iap_tx_pump();  // backstop: flush any reports queued outside callbacks
        iap_watchdog();  // handshake stall diagnostic
        attach_poll();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    }
}

bool ipod_usb_init(void) {
#ifdef CONFIG_IPOD_USB_ENABLE
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(serial_str, sizeof(serial_str), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    iap_set_serial(serial_str);

    ring = heap_caps_malloc(RING_FRAMES * 2 * sizeof(int16_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) {
        ESP_LOGE(TAG, "PSRAM ring alloc failed");
        return false;
    }
    // NOTE: PHY + TinyUSB stack start from the USB task after the boot
    // delay (see tusb_task). Starting them here would attach immediately and
    // reintroduce the cold-boot race; calling tud_task() before init would
    // block forever with no SOF events. So: allocate first, start later.
    if (xTaskCreate(tusb_task, "ipod_usb", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    usb_ready = true;
    ESP_LOGI(TAG, "iPod USB ready: 05AC:1261 UAC1 44.1k/48k + HID iAP");
    return true;
#else
    ESP_LOGI(TAG, "iPod USB disabled by Kconfig");
    return true;
#endif
}

void ipod_usb_get_status(ipod_usb_status_t *out) {
    out->boot_ms = (uint32_t) (boot_us / 1000);
    out->phy_ready_ms = (uint32_t) (phy_ready_us / 1000);
    out->first_connect_ms = (uint32_t) (first_connect_us / 1000);
    out->mount_ms = (uint32_t) (mount_us / 1000);
    out->connect_attempts = connect_attempts;
    out->usb_ready = usb_ready;
    out->host_mounted = host_mounted;
    out->audio_streaming = audio_streaming;
    out->usb_suspended = usb_suspended;
    out->tone_on = tone_on;
    out->usb_rate = usb_rate;
    out->pcm_underruns = pcm_underruns;
    out->iap_rx_packets = iap_rx_packets();
    out->iap_tx_packets = iap_tx_packets();
}

void ipod_usb_log(const char *fmt, ...) {
    char tmp[IAP_LOG_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&iap_log_lock);
    strncpy(iap_log[iap_log_head % IAP_LOG_LINES], tmp, IAP_LOG_LINE - 1);
    iap_log[iap_log_head % IAP_LOG_LINES][IAP_LOG_LINE - 1] = 0;
    iap_log_head++;
    portEXIT_CRITICAL(&iap_log_lock);
}

size_t ipod_usb_read_iap_log(char *buf, size_t buf_len) {
    size_t used = 0;
    portENTER_CRITICAL(&iap_log_lock);
    unsigned total = iap_log_head < IAP_LOG_LINES ? iap_log_head : IAP_LOG_LINES;
    unsigned start = iap_log_head < IAP_LOG_LINES ? 0 : iap_log_head % IAP_LOG_LINES;
    for (unsigned i = 0; i < total && used + 1 < buf_len; i++) {
        const char *line = iap_log[(start + i) % IAP_LOG_LINES];
        size_t n = strlen(line);
        if (used + n + 1 >= buf_len) break;
        memcpy(buf + used, line, n);
        used += n;
        buf[used++] = '\n';
    }
    portEXIT_CRITICAL(&iap_log_lock);
    if (used < buf_len) buf[used] = 0;
    return used;
}
