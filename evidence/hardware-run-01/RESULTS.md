# First hardware results

ESP32-S3 revision 0.2; 4 MB flash, 2 MB PSRAM; ESP-IDF 5.1.6.

| Case | Init / enable modes | Observed result |
|---|---|---|
| 0 | BLE / BLE | Init, enable, disable, deinit: ESP_OK |
| 1 | Classic / Classic | Init: ESP_ERR_NOT_SUPPORTED (0x106) |
| 2 | Dual / Dual | Init: ESP_ERR_NOT_SUPPORTED (0x106) |
| 3 | BLE / Classic | Init: ESP_OK; enable: ESP_ERR_INVALID_ARG (0x102); deinit: ESP_OK |
| 4 | BLE / Dual | Init: ESP_OK; enable: ESP_ERR_INVALID_ARG (0x102); deinit: ESP_OK |

All five logs contain the correct case identity, successful NVS initialization and completion marker. All flashed images passed esptool hash verification. No RF packet or discovery was tested.

The initial BOOT-held connection required a physical reset after flashing; later software resets worked. Native USB secondary console output was captured successfully. The firmware uses a 2 MB layout on the 4 MB device; the boot size warning is expected.

A complete original 4 MB flash backup is saved as original-flash.bin, excluded from Git; its hash and board identity are in board.json. The board is left with case 4 installed, its controller deinitialized. No eFuses were changed.

Conclusion: the supported controller rejects BR/EDR exactly where the source predicted. This establishes the SDK boundary, not physical impossibility of undocumented raw-radio operation. Next research is the low-level modem/test-mode path.
