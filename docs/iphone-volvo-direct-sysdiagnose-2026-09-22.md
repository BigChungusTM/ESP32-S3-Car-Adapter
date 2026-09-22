# Genuine iPhone-to-Volvo sysdiagnose, 2026-09-22

## Scope

This report covers the iPhone sysdiagnose captured at 16:55 BST while the phone
was connected directly to the 2011 Volvo USB input. It is compared with the
ESP32 emulation trace and the earlier AirPlay sysdiagnose.

The archive does not contain raw USB URBs or raw iAP command payloads. It does,
however, contain the iPhone's internal accessory, iAP1, digital-audio, routing,
and playback state transitions.

## Confirmed connection identity

At 16:55:16.117 the phone detects the physical attachment. At 16:55:17.028,
iOS identifies the connection as:

- transport: **USB Device Mode**
- protocol: **iAP1**
- accessory class: **iAP1 dock accessory**
- advertised/supported lingoes: **`0x00000411`**
- USB current limit: **500 mA**

`0x411` has bits 0, 4, and 10 set. This corresponds to the three protocol
families needed by this project:

- General Lingo (`0x00`)
- Extended Interface/Extended Remote Lingo (`0x04`)
- Digital Audio Lingo (`0x0A`)

This is strong confirmation that the intended ESP32 personality is the right
one. The Volvo does not use mass storage for a connected iPhone; it creates an
iAP1 dock endpoint with digital audio and remote-control support.

## Genuine-device timeline

| Time (BST) | Event |
| --- | --- |
| 16:55:16.117 | USB accessory attachment detected. |
| 16:55:17.028 | iAP1 protocol detected. |
| 16:55:17.039 | Accessory published to `iapd` as an iAP1 dock over USB Device Mode. |
| 16:55:17.045 | iOS records supported lingoes `0x411`. |
| 16:55:18.180 | iAP audio-device state-change notification is issued. |
| 16:55:18.189 | Digital-audio sample rate is set to 44,100 Hz successfully. |
| 16:55:19.709 | Media player validation completes. |
| 16:55:20.555 | `iapd` sends a Play command, confirming Volvo autoplay behaviour. |
| 16:55:22.706 | Supported digital-audio rates are queried. |
| 16:55:22.714 | 44,100 Hz is selected again successfully. |
| 16:55:22.761 | The system audio route changes to the dock. |
| 16:55:22.863 | Dock route is active, unmuted, with volume control available. |
| 16:55:50.714 | Cable/device detaches after a 33.7-second connection. |

The Music diagnostics identify the final route as:

- route type: **USBAudio**
- route name: **Dock Connector**
- format: **44.1 kHz, 16-bit, stereo**

## Authentication finding

iOS also starts a 30-second `AppleIDBus` transport-authentication timer when
the USB-C/Lightning accessory connection appears. That timer expires at
16:55:46 and logs `AUTH [FAILED]` / `authStatus [Timeout]`.

This is not the gate for legacy iAP1 digital audio:

- iAP1 was detected at 16:55:17;
- the dock audio sample rate was accepted at 16:55:18;
- autoplay was requested at 16:55:20;
- the USB audio route became active at 16:55:22;
- the transport-auth timeout did not occur until 16:55:46.

The connection object was ultimately destroyed with `authenticated: NO`, yet
its iAP1 dock and USB-audio endpoint had already operated. Modern AppleIDBus
authentication and legacy iAP1 command authentication must therefore be kept
separate in our reasoning.

The sysdiagnose does not expose the raw General-Lingo `0x14`–`0x19` packets, so
it cannot show the exact genuine `0x17` challenge bytes or HID fragmentation.

## Comparison with the ESP32

The ESP32 already matches several genuine behaviours:

- USB Audio Class source plus iAP HID
- iAP1 rather than iAP2
- General, Extended Interface, and Digital Audio lingoes
- 44.1 kHz, 16-bit, stereo digital audio
- a device-side USB endpoint consumed by the car

The timing and state progression differ:

- Genuine iOS exposes a usable iAP audio device about **1.15 seconds** after
  iAP1 detection.
- Genuine iOS receives an autoplay request about **3.5 seconds** after iAP1
  detection.
- The ESP32 spends extra time requesting `AccessoryInfo` (`0x27`), waiting for
  `RetDevAuthenticationSignature` (`0x18`), and then entering a compatibility
  fallback.
- The Volvo accepts the ESP32 Digital Audio attributes but never proceeds into
  the same visible media-player/autoplay interaction seen by `iapd`.

This shifts the likely remaining failure away from AirPlay and UAC packet
delivery. The strongest candidates are now:

1. the ESP32's General-Lingo authentication state is not internally coherent;
2. the extra `0x27` and delayed/fabricated authentication completion differ
   from the genuine phone's fast state progression;
3. the Extended Interface virtual library/player is not ready or coherent when
   the Volvo begins its autoplay and browsing sequence;
4. the car consequently labels the source unreadable even though it has
   accepted the audio format and scheduled isochronous transfers.

## Recommended next firmware experiment

Keep the known-good USB descriptor and UAC endpoint fix. Make the next build a
strict `0x411` iAP1 dock profile with a short deterministic startup:

1. acknowledge `IdentifyDeviceLingoes`;
2. complete or bypass the optional accessory-auth branch without `0x27`;
3. expose Digital Audio immediately at 44.1 kHz;
4. initialise a one-track Extended Interface library before the first audio
   request;
5. report a stable default item (`Track 1`) even with no AirPlay session;
6. log every Extended Interface request after Digital Audio is accepted.

The important success criterion for that build is no longer just an audio ACK
or completed isochronous packets. It is seeing the Volvo begin the same
post-connection sequence represented in the genuine trace by media-player
validation, playback-queue lookup, and the autoplay Play command.

## Capture limitation

Sysdiagnose records iOS subsystem decisions rather than USB bus contents. A
byte-for-byte comparison of the genuine HID/iAP packets still requires an
inline USB analyser or transparent USB proxy between the iPhone and Volvo.
