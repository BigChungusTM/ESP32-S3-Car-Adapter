# Initial feasibility findings

Date: 2026-09-20. Scope: installed ESP-IDF package reporting 5.1.6 and linked
official documentation. This is an initial static investigation, not a completed
reverse engineering of the S3 modem.

## What the code actually rejects

In the inspected `components/bt/CMakeLists.txt`, the ESP32-S3 selects
`controller/esp32c3/bt.c` and `include/esp32c3/include/esp_bt.h`.
That shared wrapper explicitly rejects any initialization configuration whose
`bluetooth_mode` is not BLE, returning `ESP_ERR_NOT_SUPPORTED` before controller
initialization. Enable separately checks the requested mode against the
controller's mode and returns `ESP_ERR_INVALID_ARG` on mismatch.

The original ESP32 selects its own controller wrapper. The S3's `btdm` naming
is therefore not evidence that the original ESP32 dual-mode implementation is
present. Removing a wrapper check would establish neither a BR packet formatter
nor a working controller and is not a useful first patch.

Official S3 documentation marks the Classic and dual-mode enum values
unsupported. Its supported radio architecture is Bluetooth LE.
[Controller API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/bluetooth/controller_vhci.html)
and [S3 overview](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/product-overview.html).

## Concrete reverse-engineering leads

The installed S3 PHY archive directory includes `libbtbb.a`, `libphy.a`,
`libbttestmode.a`, `librfate.a` and `librftest.a`. The inventory saves their
exported symbols and corresponding original ESP32 library symbols where present.
Across inventoried controller/PHY archives, the original ESP32 has 36 exported
T/W symbols containing `lmp`, `inquiry` or `page_scan`; the S3 has none matching
that filter. This is consistent with the documented controller split, but symbol
absence cannot prove that a silicon capability is absent. The package has no
separate original-ESP32 `libbtbb.a`, so comparison combines controller/PHY exports.

S3 `libbtbb.a` exports `bt_set_chn`, `bt_bb_corr_set`, `bt_bb_detect_set`,
`bt_bb_v2_tx_set` and `bt_bb_v2_rx_set`. Its disassembly contains real MMIO
accesses; these are more useful leads than enum names. In particular,
`bt_set_chn` modifies a bit at 0x6000e0c4 and passes an 8-bit argument onward to
`set_chan_freq_sw_start`. This does NOT establish the argument's units, the
supported frequency grid, settling time, or a legal standalone calling sequence.

The correlator setup accesses addresses including 0x60011064 and 0x60011068.
The TX setup accesses 0x60011018, 0x60011008 and 0x6001100c. Register writes alone
do not reveal a configurable Classic access-code detector or arbitrary TX bits.

Test-mode symbols include `rw_le_cs_set_freq`, `rw_le_cs_set_txdesc`,
`fcc_le_v9_tx_syncw` and `esp_ble_tx_func`. These warrant following their call
graphs and descriptor layouts; they may simply expose fixed BLE test packets.
We have not inferred function signatures from names or called them on hardware.

Missing evidence remains decisive: independent channel selection across the BR
grid; compatible modulation; formatter/whitening control; arbitrary access
codes; raw RX data; deterministic scheduling. Nothing inspected yet demonstrates
that all of these exist. LE 1M and BR similarity is a hypothesis to investigate,
not a usable transport. No Classic TX, RX, discovery or A2DP has been achieved.

## The car is a second independent problem

User observation is our best present evidence: the 2011 XC60 accepts a wired
iPhone as an iPod and displays track/time information. A menu labelled USB does
not identify the USB class or control protocol. Espressif's USB peripheral support
does not establish that generic USB Audio or Mass Storage will reproduce it.

Volvo's 2011 model information confirms USB/iPod support for relevant audio
systems, but does not publish the transaction-level protocol.
[Volvo 2011 XC60](https://www.media.volvocars.com/global/en-gb/models/volvo-xc60/2011).
Apple describes iAP and authentication within its accessory ecosystem;
requirements for a device impersonating an iPod to this particular head unit
remain unverified. We must not assume an authentication chip is either required
or sufficient for our emulation role.
[Apple accessory security](https://support.apple.com/en-ie/guide/security/sec70a4f377d/1/web/1).

A2DP audio also does not itself supply remote media commands and metadata;
those need a control path (normally AVRCP on a Classic phone connection).
The eventual adapter must translate the car's control protocol as well as audio.
No phone-side BLE audio route or transparent Wi-Fi replacement is assumed.
