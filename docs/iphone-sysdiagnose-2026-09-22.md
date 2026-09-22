# iPhone sysdiagnose: Volvo AirPlay test, 2026-09-22

## Scope

The source archive is the iPhone sysdiagnose captured at 16:39 BST after the
in-car test at approximately 16:30. The useful AirPlay session appears in the
unified log from 16:38:59 to 16:39:13.

This analysis deliberately excludes unrelated device, account, Bluetooth, and
network details from the archive.

## Result

The iPhone established and used a healthy real-time AirPlay session with the
ESP32 receiver. The capture does not show a sender-side AirPlay failure that
could explain the Volvo's `USB unreadable` state or the lack of sound from the
car.

The remaining fault is downstream of the AirPlay receiver, in the emulated
iPod/iAP state presented to the Volvo or in the USB Audio Class stream as
interpreted by the Volvo.

## Session timeline

| Time (BST) | Event |
| --- | --- |
| 16:38:57.457 | iPhone network changes to the ESP32 SoftAP; router is `192.168.4.1`. |
| 16:38:59.193 | `Volvo AirPlay` is discovered. |
| 16:38:59.729 | iPhone starts endpoint activation. |
| 16:38:59.833 | Activation succeeds over infrastructure Wi-Fi in 102 ms. |
| 16:38:59.837 | Transport format is selected: ALAC, 44.1 kHz, 16-bit, stereo, 352 samples/packet. |
| 16:38:59.911 | Audio `StartIO` begins. |
| 16:39:01.952 | Remote audio stream and UDP transport to `192.168.4.1` are established. |
| 16:39:01.956 | `StartIO` completes; the stream enters MediaPlaying. |
| 16:39:01.983 | Sender flushes/anchors the audio stream and begins normal real-time delivery. |
| 16:39:07–11 | Sender buffer remains stable at about 43%, representing about 1.77 seconds. |
| 16:39:12.085 | The local now-playing client disappears and playback state becomes unknown. |
| 16:39:12.872 | iOS suspends the audio endpoint cleanly. |
| 16:39:13.099 | Session ends with zero retransmits and zero futile retransmits. |
| 16:39:16 | The phone leaves the ESP32 Wi-Fi network. |

## Audio and transport evidence

- iOS encoded the stream as **ALAC / 44,100 Hz / 16-bit / stereo**.
- Each AirPlay packet represented **352 audio frames**.
- Negotiated presentation latency was **2,000 ms**.
- The receiver was reached through ordinary infrastructure Wi-Fi (`en0`),
  from `192.168.4.2` to `192.168.4.1`; this was not an AWDL session.
- UDP transport setup completed successfully and the sender continued emitting
  time announcements and maintaining a stable buffer.
- Final sender diagnostics report:
  - total retransmits: 0
  - futile retransmits: 0
  - primary packet drops: 0
  - I/O discontinuities: 0
  - AirPlay core-capture triggers: 0
- The receiver was not muted. Initial volume was -20 dB and iOS sent subsequent
  volume changes normally.

The session shut down because the phone's local now-playing client was
invalidated, followed by a normal suspension and teardown. It did not end due
to an AirPlay transport timeout or packet-loss failure.

## Log messages that are not the primary failure

The capture includes `BufferedAudio stream not found`, missing URL playback
tokens, unsupported audio-mode properties, and Bonjour link lookup errors.
The selected endpoint is a real-time audio receiver rather than a buffered URL
playback target, and the session subsequently streams normally. These messages
therefore do not identify the Volvo silence.

There are also failed BTLE attempts to an unrelated previously known AirPlay
device. They do not involve the ESP32 endpoint.

## iAP observations

The iPhone posts now-playing notifications to `com.apple.iapd`, but this is an
internal iOS media integration path. The phone was not physically connected to
the Volvo during this capture, so the archive contains no USB transaction trace
of a genuine iPhone/iPod performing iAP1 authentication with the car.

The sysdiagnose therefore cannot answer the outstanding USB question: which
exact iAP1 command or state causes the Volvo to accept the virtual iPod instead
of displaying `USB unreadable`.

## Engineering implication

Freeze the working AirPlay negotiation while diagnosing the Volvo path. The
next useful capture must come from the ESP32 and should correlate these events
on one timeline:

1. iAP authentication status, including whether `0x19` is transmitted before
   the Digital Audio lingo starts.
2. Volvo selection or rejection of the audio streaming alternate setting.
3. Every UAC isochronous packet length and completion result.
4. PCM ring reads, zero-fill frames, underruns, and samples actually submitted
   to TinyUSB.
5. The exact moment the head unit changes from `iPod loading` to
   `USB unreadable`, including any HID/iAP traffic immediately before it.

For a true reference iAP1 trace, software on the iPhone is insufficient: the
USB traffic between a genuine iPhone/iPod and the Volvo must be captured with
an inline USB protocol analyser or a purpose-built transparent USB proxy.

## Decoder note

The EC-DIGIT Sysdiagnose Analysis Framework is useful for repeatable structured
extraction, but its logarchive support ultimately invokes Apple's `log show` on
macOS. Native `log show` was used here so the original unified-log events could
be filtered directly without installing a second parser.
