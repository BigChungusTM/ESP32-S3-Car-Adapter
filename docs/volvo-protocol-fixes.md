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

## September 21 car capture: stalled at section 3 of 7

The prior build was subsequently flashed and hash-verified. The supplied car
capture shows one attach attempt, mount at 2961 ms, streaming alternate setting
1 and 44.1 kHz. It receives certificate sections 0, 1, 2 and 3 (maximum index 7),
then stalls. No signature request/response has occurred. Transactions are absent
in this legacy identification flow, so `trx=absent/0000` is not evidence of the
Rockbox transaction-echo bug. The displayed latency measures response enqueue
time, not host receipt. Zero completed audio packets does not establish why
authentication stopped.

Source inspection found the Go HID encoder zero-pads to each declared report
length, while our transmitter sent only the used bytes. The next build pads
reports to 12/14/20/63 bytes (excluding report ID), logs actual HID completion
bytes and includes queue depth in stall diagnostics. Host tests enforce report
sizes and zero padding. This is a reference-fidelity correction and diagnostic
experiment; it is not yet a confirmed fix for the car's failure.

## Follow-up: full certificate, no signature response

The padded build reached section 7/7 (945 bytes). USB completion logs show 0x16
and 0x17 delivered, but no 0x18 before the vehicle reported unreadable. The user
confirmed this was a complete failed cycle, not merely the two-second watchdog.

Apple's public [protocol description in US7293122B1](https://patents.google.com/patent/US7293122B1/en)
describes accessory challenge/signature/status exchanges, with restricted
commands enabled after successful authentication. It is historical explanatory
material, not a complete normative specification for this firmware. Apple's
[MFi programme](https://mfi.apple.com/en/how-it-works) provides the detailed
specifications to licensees; no authenticated copy of the applicable legacy
firmware specification was obtained in this investigation.

Direction matters: 0x14–0x19 here authenticate the Volvo accessory to our
emulated iPod. No received command in this capture requests proof of the S3's
Apple-device identity. Missing Apple-device credentials are therefore not an
established explanation for this stall.

Independent implementation comparison found a specific mismatch:

| Source | V2 challenge | Trailing retry counter |
| --- | --- | --- |
| oandrew/ipod General handler | 20 zeros | 0 |
| Rockbox upstream iap-core.c, AUST_CERTDONE | 20 RX-buffer bytes | 1 |
| Rockbox ipod5g-mfi-digital-audio fork, same state | 20 RX-buffer bytes | 1 |

Source: [Rockbox core](https://github.com/Rockbox/rockbox/blob/master/apps/iap/iap-core.c),
[fork core](https://github.com/jaredbsanchez-png/rockbox/blob/ipod5g-mfi-digital-audio/apps/iap/iap-core.c).
The next local build changes only the request's trailing counter from 0 to 1
(plus its resulting checksum and diagnostic log). Challenge bytes, transaction
handling and audio sequencing stay the same to isolate this discrepancy. This
is an evidence-based compatibility test, not a claim that the specification
forbids counter zero. Tests cover eight legacy certificate sections with no
transaction fields as well as the IDPS case. Nothing in this update adds real
cryptographic verification or demonstrates successful car authentication.
