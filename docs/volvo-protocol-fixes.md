# Volvo USB/iAP investigation

Local branch: `fix/volvo-ipod-protocol`. Hardware validation is still required.

## What changed

- Initialise USB disconnected, delay attachment, and service TinyUSB without an
  indefinite wait so enumeration retry timers can run. Wi-Fi startup waits for
  mount or a bounded timeout, preserving standalone SoftAP diagnostics.
- Track multipart authentication certificates, reject missing/out-of-order or
  inconsistent sections, and ACK identical retransmissions without restarting
  negotiation. Request the signature after the final section. Start DigitalAudio
  once after the signature response; READY requires a successful audio-attributes
  ACK.
- Preserve response transaction IDs and log their numeric values. The signature
  request remains 21 zero bytes: the Go reference really serializes a 20-byte
  challenge and one-byte counter. Its final-certificate response transaction is
  now preserved instead of allocating a notification transaction.
- Keep VID/PID 05AC:1261 and configuration value 2. Expose descriptor index 0
  only, fix TinyUSB's configuration-value lookup, associate audio terminals, and
  correct the AudioControl class-specific total length to 30 bytes.
- Retain partially accepted audio batches. Generate USB packets with fractional
  pacing: 441 frames per ten packets at 44.1 kHz, 480 at 48 kHz. Fill starvation
  with silence and count it separately. Distinguish received, buffered, dropped
  and successfully completed USB frames. Playback position uses completed USB
  time while playback is active rather than bytes entering the ring.
- Expose a single-track AirPlay library and a small media API between RAOP and
  USB. USB still owns the PCM ring. Reverse phone controls remain future work;
  existing ExtendedRemote compatibility handlers have not all been replaced.

TinyUSB is pinned to 0.21.0~2. `cmake/patch_tinyusb.py` generates patched build
sources without editing managed dependencies. A dependency upgrade must review
these integration changes. Packet completion counters include silence; they do
not prove that the Volvo rendered audible audio.

## Reference findings and the Reddit report

The [Rockbox fork's General handler](https://github.com/jaredbsanchez-png/rockbox/blob/ipod5g-mfi-digital-audio/apps/iap/iap-lingo0.c)
explicitly saves the incoming transaction bytes for command 0x18 and echoes
them in 0x19. It also defers DigitalAudio to avoid overwriting its shared DMA
transmit buffer. This implementation queues copies, so that particular buffer
hazard is not the same.

The screenshots describe a fabricated **transaction ID** 0x0000, not a command
named 0x0000. Here lingo 0x0A command 0x00 is AccAck. For example:

```
<< lingo=00 cmd=0018 trx=0045 ...
>> lingo=00 cmd=0019 trx=0045 ...
<< lingo=0A cmd=0000 trx=0046 ...
AccAck status=0 cmd=0x04
```

The first two lines must preserve the transaction. The last two can be a normal
successful audio ACK. A zero transaction is not intrinsically invalid; it must
match the exchange. No current Volvo raw capture establishes that the Rockbox
bug was this project's failure.

The checked-in `research/ipod` reference starts DigitalAudio after the final
certificate, before signature completion. Waiting until the signature response
in this branch is a deliberate sequencing experiment, not an exact copy of that
reference. Neither this implementation nor the examined Rockbox handler verifies
the certificate/signature cryptographically; an ACK is protocol progress only.

## Validation and next car test

Host tests pass with AddressSanitizer and UndefinedBehaviorSanitizer: certificate
ordering/retransmissions, challenge wire bytes, nonzero transaction echo, single
audio start, playback clock, packet totals and partial-write retention. The
ESP-IDF ESP32-S3 firmware builds. These tests do not exercise real USB timing.

1. Flash the complete build using `idf.py -p PORT flash` from
   `firmware/airplay-receiver` after sourcing `scripts/idf-env.sh` at repo root.
2. Cold-power the board from the Volvo without holding BOOT. Record mount,
   alternate interface, selected rate, and the full iAP log before restarting.
3. Check that certificate sections precede 0x17, then 0x18/0x19 preserve the
   transaction, followed by one 0x0A/0x02 request and successful AccAck for 0x04.
4. Use the existing tone mode to separate USB audio from AirPlay reception.
   Compare completed-frame deltas over ten seconds: approximately 441,000 at
   44.1 kHz or 480,000 at 48 kHz. Investigate FIFO-silence and dropped-frame growth.
5. Test AirPlay over the board's SoftAP, then pause, resume, track changes and
   another cold start. Separately verify iPhone cellular streaming availability.

No board was available on a serial port during these checks. Nothing was flashed
or pushed to GitHub as part of this change.
