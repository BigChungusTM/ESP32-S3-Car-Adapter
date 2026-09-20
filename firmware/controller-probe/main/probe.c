#include <stdio.h>
#include "sdkconfig.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "This probe is specifically for ESP32-S3"
#endif

static void result(const char *operation, esp_err_t err)
{
    printf("PROBE operation=%s error=%s code=0x%x status=%d\n",
           operation, esp_err_to_name(err), (unsigned)err,
           (int)esp_bt_controller_get_status());
}

void app_main(void)
{
    // One case per boot: never switch controller modes after enable.
    const esp_bt_mode_t init_modes[] = {
        ESP_BT_MODE_BLE, ESP_BT_MODE_CLASSIC_BT, ESP_BT_MODE_BTDM,
        ESP_BT_MODE_BLE, ESP_BT_MODE_BLE
    };
    const esp_bt_mode_t enable_modes[] = {
        ESP_BT_MODE_BLE, ESP_BT_MODE_CLASSIC_BT, ESP_BT_MODE_BTDM,
        ESP_BT_MODE_CLASSIC_BT, ESP_BT_MODE_BTDM
    };
    printf("PROBE target=esp32s3 idf=%s case=%d init_mode=%d enable_mode=%d\n",
           esp_get_idf_version(), PROBE_CASE, init_modes[PROBE_CASE],
           enable_modes[PROBE_CASE]);
    esp_err_t err = nvs_flash_init();
    result("nvs_init", err);
    // Deliberately do not erase NVS if it is incompatible.
    if (err != ESP_OK) {
        printf("PROBE done=1 stage=nvs_failure\n");
        return;
    }
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    cfg.bluetooth_mode = init_modes[PROBE_CASE];
    err = esp_bt_controller_init(&cfg);
    result("controller_init", err);
    if (err != ESP_OK) {
        printf("PROBE done=1 stage=init_failure\n");
        return;
    }
    err = esp_bt_controller_enable(enable_modes[PROBE_CASE]);
    result("controller_enable", err);
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_disable();
        result("controller_disable", err);
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        err = esp_bt_controller_deinit();
        result("controller_deinit", err);
    }
    printf("PROBE done=1 stage=complete\n");
}
