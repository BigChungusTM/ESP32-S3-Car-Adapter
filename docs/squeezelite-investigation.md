# squeezelite-esp32 investigation

Repository: https://github.com/sle118/squeezelite-esp32

Inspected branch: master-v4.3. Commit:
`1d542bd53cf397661f11c0671553422acdb663ab` (2026-07-30).
Shallow source checkout: `../research/squeezelite-esp32`.
Submodules were not initialized. No firmware from this repository was built or
flashed; the board remains on our controller probe.

## Finding

Useful for an AirPlay-over-Wi-Fi input, not a solution for Bluetooth audio on the
S3. Its README identifies AirPlay 1, S3 support using IDF 4.4, and explicitly
excludes S3 Bluetooth audio. The full project's stated baseline is 4 MB PSRAM;
our attached chip has 2 MB. A reduced implementation would need memory profiling.
[README](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/README.md)

## Source evidence

- `components/driver_bt/CMakeLists.txt:1`: builds the Bluetooth component only
  when `IDF_TARGET` equals `esp32`.
- `components/driver_bt/bt_app_core.c:92`: enables
  `ESP_BT_MODE_CLASSIC_BT`. This is ordinary Espressif Classic support, not BLE
  speaker emulation or a replacement BR controller.
- `components/driver_bt/bt_app_sink.c:676`: initializes the A2DP sink.
- `components/raop/rtp.c:412`: decodes ALAC to PCM; the playback loop supplies
  decoded frames through `ctx->data_cb` with playback timing.
- `components/squeezelite/decode_external.c:205`: receives the AirPlay PCM callback
  and feeds the shared audio sink. This is a potential integration point for a
  future USB audio output, but buffering and clock synchronization must survive
  that change.
- `components/raop/raop.c:634` and `raop_sink.c:133`: receive and handle artist,
  album and title; progress events carry elapsed/duration information.
- `components/raop/raop.c:275`: maps commands to remote play, pause, next,
  previous, and volume requests. The HTTP request includes Active-Remote, and
  requires a discovered remote endpoint. Code presence is not proof that every
  current iPhone app will accept these commands.
- `components/raop/raop_sink.c:186`: starts AirPlay from connected Wi-Fi/Ethernet
  state callbacks. The stock setup access point is not evidence of a ready-made
  standalone in-car AirPlay network.
- `components/squeezelite/output_embedded.c:75`: selects Bluetooth output or
  I2S/SPDIF. Inspection/search of main and component C/H/CMake sources found no
  USB iPod/iAP emulation or TinyUSB audio-device implementation.

Source links:
[Bluetooth build gate](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/driver_bt/CMakeLists.txt),
[Classic controller setup](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/driver_bt/bt_app_core.c),
[RAOP implementation](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/raop/raop.c),
[PCM integration](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/decode_external.c).

## Application to this project

Candidate architecture: iPhone AirPlay -> Wi-Fi -> S3 RAOP decoder -> buffered
PCM and metadata -> new Volvo USB/iPod implementation. Car playback commands
would be translated into AirPlay remote-control requests where supported.

This preserves normal phone-app playback in principle, using an AirPlay output
selection instead of Bluetooth pairing. It does not require a bespoke phone app
to extract another app's audio. Compatibility with the actual iPhone/iOS and
source apps still requires testing.

The two independent experiments should be:

1. Minimal S3 AirPlay receiver: initially use an ordinary shared Wi-Fi network;
   establish discovery, PCM reception, metadata, remote commands and memory use.
   Use a small bounded PCM capture or an I2S output as evidence, without involving
   the car. Avoid loading the full player and its unused services into 2 MB PSRAM.
2. Volvo USB/iPod emulation: separately establish enumeration, audio transport and
   control protocol with known-good wired behavior. This repository does not
   supply that implementation.

Then test in-car network topology (phone hotspot or adapted S3 AP), discovery,
cellular internet access, locking/background operation, buffering delay and
automatic reconnection. None of those are established by finding AirPlay code.

Recommendation: this is a substantially more concrete S3-only audio-input lead
than undocumented BR modem work, if Wi-Fi/AirPlay selection is acceptable.
