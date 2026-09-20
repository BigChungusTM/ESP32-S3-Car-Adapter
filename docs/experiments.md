# Experiments and acceptance criteria

## M0 — Supported controller boundary

Build all five probe images. On a spare board, save every boot log and identify
board/module, serial port, firmware case and IDF version. BLE must initialize and
enable successfully for the unsupported-mode failures to be meaningful.
Unexpected results trigger source/configuration review; they do not establish
Classic radio support. Status: all five cases completed on the first board and matched expectations.
See `../evidence/hardware-run-01/RESULTS.md`.

## M1 — Determine whether a raw modem path exists

Start with the saved library inventory and follow test-mode TX descriptors,
frequency-setting routines and correlator setup. Recover prototypes from real
call sites and map register fields before constructing a board experiment.
Compare original ESP32 and S3 semantics, not just similarly named functions.

For each capability record: observed code, inferred meaning, alternative
explanation, and the physical measurement that could distinguish them.
Required capabilities are frequency grid/settling, modulation control, arbitrary
packet bits without mandatory BLE formatting, RX access-code behavior, and timing.
If only fixed BLE test framing is accessible, this path remains blocked.

## M2 — One independently verified BR transmission

Agree an exact packet type and reference bit sequence first. Inquiry ID packets
and inquiry-response FHS packets are different; an arbitrary access-code/header
packet is not automatically a discoverable device. Verify waveform frequency,
symbol timing and decoded bits with suitable independent equipment. A BLE
scanner, a carrier peak, or a self-decoded S3-to-S3 packet is insufficient.

Current equipment does not provide that measurement. Further static research and
controller tests can proceed now. Before buying anything, investigate whether the
Mac's built-in Bluetooth and its available diagnostic tools expose usable Classic
inquiry evidence; this has not been checked. An iPhone device-list sighting would
be a preliminary observation unless its transport can be independently established.

## M3 — Genuine Classic inquiry response (first major milestone)

Receive a real BR inquiry and transmit a valid, correctly timed response using
only the S3 radio. An independent Classic controller must report its address and
inquiry result. Repeat with the S3 powered off as a negative control, then on,
using an attributable address. Preserve controller logs or a decoded RF capture.
This requires more than TX alone and does not yet prove pairing or audio.

## M4 — Connection, audio and controls

Only after radio evidence: paging/connection, hopping, LMP/security, HCI ACL,
L2CAP/SDP, AVDTP/A2DP sink and AVRCP control/metadata. Prove stable phone audio
separately from the car before joining the two halves.

## Independent car investigation

Record exact audio-system variant and the working phone/cable arrangement.
Establish descriptors, enumeration and control/audio exchanges from a known-good
wired session using a suitable USB capture setup. A spare S3 is not assumed to
be a transparent USB sniffer. Identify the relevant iAP generation and audio
transport from evidence. First car-side success is recognized identity plus a
test audio stream and play/pause/track commands; then metadata and reconnection.

If the S3 modem cannot expose BR, preserve these findings. An external Classic
controller or a separate supported wireless transport would be a new design
decision, not a claimed success of the S3-only experiment.
