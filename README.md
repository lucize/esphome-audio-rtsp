# ESPHome Audio RTSP

Minimal ESPHome external component that exposes an ESPHome `microphone:` entity as an audio-only RTSP stream.

It is intended for ESP32 + I2S microphones such as the INMP441, but the RTSP component itself no longer owns the I2S peripheral. The normal ESPHome `i2s_audio:` and `microphone:` components configure and read the microphone; `rtsp_audio:` consumes that microphone stream and packetizes it as RTP/L16 over RTSP.

> This project was vibecoded by ChatGPT and Claude, with testing and iteration on real ESPHome/ESP32 hardware.

## Status

Experimental. It is a minimal audio-only RTSP server, not a full camera/media framework.

Supported today:

- ESP32 with `framework.type: esp-idf`
- ESPHome `microphone:` input
- RTSP control over TCP
- RTP audio over UDP
- L16 / signed 16-bit PCM / mono
- One active RTSP client at a time
- Optional RTSP Basic authentication


Not supported yet:

- RTSP interleaved TCP RTP
- AAC/Opus encoding
- Multiple simultaneous RTSP clients

## Runtime behavior and optimizations

The component intentionally uses ESPHome's native `i2s_audio:` / `microphone:` stack instead of opening the I2S peripheral itself. The RTSP server starts at boot, but microphone capture is started only while an RTSP client is in `PLAY` state and stopped again when streaming ends.

Current low-risk optimizations:

- RTP sender task is separate from the RTSP control task.
- Microphone callbacks never block; overflow is counted as `drop=` in debug logs.
- The audio buffer trigger is aligned to one RTP packet instead of waking on tiny partial chunks.
- The audio buffer is reset on `PLAY` to avoid stale buffered audio and reduce initial delay.
- RTSP control sockets use `TCP_NODELAY`.
- RTP UDP send buffer is enlarged to reduce short Wi-Fi/lwIP backpressure bursts.
- RTP send warnings are throttled so logging does not make audio glitches worse.

Recommended stable defaults:

```yaml
packet_ms: 20
buffer_ms: 200
gain_factor: 4
```

Lower `buffer_ms` can reduce latency but increases the chance of dropouts on weak Wi-Fi. Higher `buffer_ms` can smooth jitter but adds delay.

## Credits / inspiration

Inspired by Phil Schatzmann's Arduino Audio Tools RTSP example:

- [Arduino Audio Tools](https://github.com/pschatzmann/arduino-audio-tools)
- [communication-audiokit-rtsp.ino example](https://github.com/pschatzmann/arduino-audio-tools/blob/main/examples/examples-communication/rtsp/communication-audiokit-rtsp/communication-audiokit-rtsp.ino)

The first proof of concept used Arduino Audio Tools to validate the idea. This repository is a separate ESPHome / ESP-IDF implementation and does not use Arduino Audio Tools at runtime.

## Install from GitHub

Use the repository as an ESPHome external component:

```yaml
external_components:
  - source: github://lucize/esphome-audio-rtsp@main
    components: [rtsp_audio]
    refresh: 0s
```

Alternative explicit git syntax:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/lucize/esphome-audio-rtsp.git
      ref: main
    components: [rtsp_audio]
    refresh: 0s
```

For normal long-term use you can remove `refresh: 0s` after the component is stable. Keeping it during development avoids ESPHome using a cached older schema.

## Example configuration

```yaml
esphome:
  name: mic-front
  friendly_name: mic-front

external_components:
  - source: github://lucize/esphome-audio-rtsp@main
    components: [rtsp_audio]
    refresh: 0s

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:
  level: DEBUG
  logs:
    rtsp_audio: DEBUG

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  power_save_mode: none

captive_portal:

i2s_audio:
  - id: i2s_in
    i2s_lrclk_pin: GPIO25   # INMP441 WS / LRCLK
    i2s_bclk_pin: GPIO26    # INMP441 SCK / BCLK

microphone:
  - platform: i2s_audio
    id: inmp441_mic
    i2s_audio_id: i2s_in
    i2s_din_pin: GPIO27     # INMP441 SD / DOUT
    adc_type: external
    pdm: false
    sample_rate: 16000
    bits_per_sample: 16bit
    channel: left           # INMP441 L/R wiring; try right if silent

rtsp_audio:
  microphone: inmp441_mic
  port: 8554
  audio_channel: 0          # microphone output channel index; mono mic = 0
  gain_factor: 4            # 1-64 integer gain
  rtp_payload_type: 96
  packet_ms: 20
  buffer_ms: 200
  debug: true
  status_interval: 10s

  # Optional RTSP Basic authentication. Omit both to disable auth.
  username: !secret rtsp_username
  password: !secret rtsp_password
  auth_realm: ESPHome RTSP Audio
```

## RTSP URL examples

Use your actual ESPHome device IP or DNS name. The examples intentionally use placeholders instead of a private LAN IP:

```text
rtsp://<esphome-device.lan>:8554/
rtsp://<esphome-device>.local:8554/
rtsp://<esp32-ip>:8554/
```

For documentation or templates where the address is not known yet, use:

```text
rtsp://0.0.0.0:8554/
```

Do not use `0.0.0.0` as the real client URL. It only means "bind/listen on all local interfaces" in server-side examples; clients must connect to the ESP32's actual IP or hostname.
## Test with ffplay

This component currently supports RTP over UDP. Do not force RTSP-over-TCP/interleaved transport.

```bash
ffplay rtsp://<esp32-ip>:8554/
```

or:

```bash
ffplay rtsp://mic-front.local:8554/
```

## Configuration reference

### `rtsp_audio:`

| Option | Required | Default | Description |
|---|---:|---:|---|
| `microphone` | yes | | ID of the ESPHome `microphone:` entity to stream. |
| `port` | no | `8554` | RTSP TCP control port. |
| `audio_channel` | no | `0` | Output channel index from the microphone source. For a mono INMP441 mic, use `0`. |
| `gain_factor` | no | `4` | Integer microphone gain factor, 1-64. |
| `rtp_payload_type` | no | `96` | Dynamic RTP payload type used for L16 audio. |
| `packet_ms` | no | `20` | Audio per RTP packet, in milliseconds. |
| `buffer_ms` | no | `200` | Microphone-to-RTP buffer size in milliseconds. Lower reduces latency; higher tolerates more jitter. |
| `debug` | no | `false` | Enables periodic status/debug logging. |
| `status_interval` | no | `10s` | Debug status log interval. |
| `username` | no | | RTSP Basic auth username. Must be set together with `password`. |
| `password` | no | | RTSP Basic auth password. Must be set together with `username`. |
| `auth_realm` | no | `ESPHome RTSP Audio` | Realm shown in the RTSP `WWW-Authenticate` challenge. |

## Testing

Use UDP RTSP:

```bash
ffplay rtsp://<esphome-device.lan>:8554/
```

or VLC:

```text
rtsp://<esphome-device.lan>:8554/
```

Do not force TCP transport:

```bash
ffplay -rtsp_transport tcp rtsp://<esphome-device.lan>:8554/
```

RTSP-over-TCP interleaved transport is not implemented yet.

## Debugging

With `debug: true`, the log includes:

```text
packets=... send_err=... clip=... i2s_reads=... empty=... bytes=... peak=... min=... max=...
```

Typical interpretation:

- `streaming=YES`, `packets` increasing, `send_err=0`: RTSP/RTP is sending.
- `clip` increasing quickly or `peak=32767`: gain/shift is too aggressive.
- `packets` and `i2s_reads` increase but `peak=0 min=0 max=0`: try `channel: right` or check INMP441 L/R wiring.
- `send_err` increasing: UDP RTP packets are not leaving the ESP32 reliably; check Wi-Fi, VLAN/firewall, or client behavior.


## Notes for Frigate/go2rtc

This component exposes an audio-only RTSP stream. Some consumers handle audio-only RTSP better than others. If direct Frigate use is unreliable, put `go2rtc` in between and let Frigate consume the go2rtc stream.


## License

GPL-3.0 license
