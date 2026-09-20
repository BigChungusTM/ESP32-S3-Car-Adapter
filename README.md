# AirPlay-to-USB-iPod bridge (ESP32-S3)

An ESP32-S3 joins AirPlay audio from an iPhone over Wi-Fi and presents itself
to a car stereo as a USB iPod (USB Audio + iAP over HID), so older head units
with an iPod/USB input can play wireless audio with track metadata.
Developed against a 2011 Volvo XC60 and iPhone 16 or newer. The car's source
menu says USB; with the phone wired in, the simple LCD identifies it as an
iPod and shows track information and elapsed time. Preserve this music/control
behavior. This project does not implement Apple CarPlay.

Configure the device name, Wi-Fi SSID and password with `idf.py menuconfig`
under "AirPlay iPod bridge identity" (defaults work out of the box for a
first test; change them for permanent use).

Hardware available: multiple ESP32-S3 boards, BLE devices; no SDR or external
Bluetooth adapter. First connected board: ESP32-S3 revision 0.2, embedded 4 MB
flash and 2 MB PSRAM, native USB Serial/JTAG. Hardware logs and flash backup are
under `evidence/hardware-run-01/`.

## First experiment

`firmware/controller-probe` tests the supported controller boundary without a
Bluetooth host, A2DP, undocumented calls, or register patches. Five independent
images make failures attributable to initialization versus enable:

| Case | Initialize | Enable | Source-derived expectation (not a board result) |
|---|---|---|---|
| 0 | BLE | BLE | Both succeed: baseline |
| 1 | Classic | Classic | Init returns ESP_ERR_NOT_SUPPORTED |
| 2 | Dual mode | Dual mode | Init returns ESP_ERR_NOT_SUPPORTED |
| 3 | BLE | Classic | Init succeeds; enable returns ESP_ERR_INVALID_ARG |
| 4 | BLE | Dual mode | Init succeeds; enable returns ESP_ERR_INVALID_ARG |

Each boot logs `PROBE` lines with IDF version, case, requested modes, return
codes and controller status, then cleans up. It does not advertise or implement
inquiry. A successful BLE initialization alone is not an RF test.

The baseline uses the existing local ESP-IDF 5.1.6 package. The package is not a
pristine Git checkout; hashes of the inspected files are in the evidence folder.
Do not confuse these findings with validation of every newer IDF release.

```bash
cd /Users/lojaws/Documents/ESP32-S3-Car-Adapter
bash scripts/build-probes.sh
```

The toolchain is isolated in `.tools`. For a fresh tool installation:

```bash
export IDF_TOOLS_PATH="$PWD/.tools"
/Users/lojaws/.platformio/packages/framework-espidf/install.sh esp32s3
# Compatibility pin for this older IDF's dependency checker:
.tools/python_env/idf5.1_py3.9_env/bin/python -m pip install \
  'ruamel.yaml==0.18.6' 'ruamel.yaml.clib==0.2.8' 'setuptools==70.3.0'
```

To flash case 0, enter bash and use the actual board's serial port:

```bash
source scripts/idf-env.sh
cd firmware/controller-probe
idf.py -B build-0 -p /dev/cu.REPLACE_WITH_BOARD_PORT flash monitor
```

Repeat with `build-1` through `build-4`, retaining the serial output in separate
files under `evidence/`. Flashing replaces the board's application. Use a spare
board. Default logging is UART0 (normally the board's USB-to-UART connector).
A native USB Serial/JTAG board can use the generated configuration's secondary
USB console; this has been confirmed on the first connected board.
Record exact board model, flash size and connection before flashing. The probe
does not automatically erase incompatible NVS.

## Research and evidence

- [Initial findings and limits](docs/feasibility.md)
- [Experiment milestones](docs/experiments.md)
- `scripts/inspect-radio.py`: repeatable symbol inventory, source snapshots,
  S3 baseband disassembly and SHA-256 manifest, all read-only.
- `evidence/build.log`: compilation record; not evidence of on-air operation.

For native USB Serial/JTAG, `scripts/run-probe.py` flashes a selected case and
captures its boot output using the project's Python environment. Example:

```bash
.tools/python_env/idf5.1_py3.9_env/bin/python scripts/run-probe.py \
  --port /dev/cu.usbmodem101 --case 0 --output evidence/new-run
```

Entering download mode by holding BOOT on connection may require one physical
RESET/EN press without BOOT after the first flash. Later software resets worked
on the tested board. The original probe images use a 2 MB flash layout within
the detected 4 MB flash; the resulting size warning does not prevent the tests.
