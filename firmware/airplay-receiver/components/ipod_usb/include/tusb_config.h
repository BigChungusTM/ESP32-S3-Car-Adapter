#pragma once
// TinyUSB stack configuration for the Volvo iPod-USB device.
// Device-only: UAC1 microphone-direction audio (ESP32 -> car) + HID iAP transport.

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_OS               OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG            0

// Enable device stack
#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#define CFG_TUD_AUDIO             1
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               1
#define CFG_TUD_HID               1
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION (UAC1, 48 kHz stereo device->host)
//--------------------------------------------------------------------+

#define IPOD_USB_SAMPLE_RATE              48000
#define IPOD_USB_CHANNELS                 2
#define IPOD_USB_BYTES_PER_SAMPLE         2
// Exact 1 ms frame at 48 kHz stereo s16: 48000*2*2/1000 = 192
#define IPOD_USB_EP_IN_SIZE               192

#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE              IPOD_USB_SAMPLE_RATE
#define CFG_TUD_AUDIO_ENABLE_EP_IN                    1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                   0
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX    IPOD_USB_BYTES_PER_SAMPLE
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX            IPOD_USB_CHANNELS
#define CFG_TUD_AUDIO_EP_SZ_IN                        IPOD_USB_EP_IN_SIZE
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL              1
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX             IPOD_USB_EP_IN_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ          (IPOD_USB_EP_IN_SIZE * 8)

//--------------------------------------------------------------------
// HID CLASS DRIVER CONFIGURATION (iAP transport, interrupt IN only)
//--------------------------------------------------------------------+

#define CFG_TUD_HID_EP_BUFSIZE                64

//--------------------------------------------------------------------+
// MSC CLASS DRIVER CONFIGURATION (FAT16 RAM disk, config 1)
//--------------------------------------------------------------------+

#define CFG_TUD_MSC_EP_BUFSIZE                512

#ifdef __cplusplus
}
#endif
