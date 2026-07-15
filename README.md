# ESPHome Audio RTSP

Minimal audio-only RTSP server for ESPHome using an ESP32 and an I2S microphone such as the INMP441.

This project is **vibecoded by ChatGPT** together with hands-on user testing, ESPHome logs, and iterative fixes. Treat it as an experimental community component, not an official ESPHome component.

## What it does

```text
INMP441 / I2S microphone
  -> ESP-IDF I2S STD RX driver
  -> signed 16-bit PCM conversion
  -> RTP L16 mono packets over UDP
  -> minimal RTSP control server
  -> VLC / ffplay / go2rtc / Frigate-compatible input, depending on client support
```

The component is intentionally small. It does not use Arduino, `WiFi.h`, Arduino AudioTools, or ESPHome's built-in `i2s_audio` / `microphone` component.

## Status

Tested baseline:

- ESPHome `2026.6.5`
- ESP32 rev3.1 / `esp32dev`
- ESP-IDF framework
- INMP441-style I2S microphone
- RTSP port `8554`

Known-good audio baseline from testing:

```yaml
use_stereo_slot: false
sample_shift: 14
gain: 4.0
packet_ms: 20
```

`use_stereo_slot: false` is important for the tested INMP441 setup. Stereo slot mode caused robotic/chopped audio during testing.

## Features

- Native ESP-IDF only
- ESP-IDF 5 I2S STD driver
- lwIP TCP RTSP listener
- lwIP UDP RTP audio streaming
- Dynamic RTP client ports from RTSP `SETUP`
- One active RTSP client
- L16 mono PCM RTP payload
- Software gain
- Configurable sample extraction shift for 32-bit I2S microphones
- Debug counters for packets, send errors, clipping, I2S reads, and peak/min/max
- ESPHome-style YAML aliases for I2S pin names

## Limitations

- RTP over UDP only
- No RTSP interleaved TCP transport yet
- No RTCP sender reports yet
- One client at a time
- Raw L16 PCM only; no AAC, Opus, or MP3 compression
- The component owns the I2S peripheral directly; do not also configure ESPHome `i2s_audio:` / `microphone:` for the same pins

## Repository layout

For ESPHome remote fetching, the component is stored under:

```text
components/rtsp_audio
```

ESPHome expects git-sourced external components to live inside a repository `components` folder or `esphome/components` folder.

## Install from GitHub

Use the repository directly from ESPHome:

```yaml
external_components:
  - source: github://lucize/esphome-audio-rtsp@main
    components: [rtsp_audio]
```

Alternative explicit git syntax:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/lucize/esphome-audio-rtsp.git
      ref: main
    components: [rtsp_audio]
```

If you publish tagged releases, pin a tag instead of `main` for reproducible builds:

```yaml
external_components:
  - source: github://lucize/esphome-audio-rtsp@v0.1.0
    components: [rtsp_audio]
```

## Local install

Clone or copy this repository into your ESPHome config directory and point ESPHome at the local `components` folder:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [rtsp_audio]
```

If replacing an older local copy, delete the old folder first:

```bash
rm -rf /config/components/rtsp_audio
```

If the ESPHome dashboard editor still shows old schema errors, restart the ESPHome add-on/container or use **Clean Build Files**.

## Full YAML example

```yaml
esphome:
  name: mic-front
  friendly_name: mic-front

external_components:
  - source: github://lucize/esphome-audio-rtsp@main
    components: [rtsp_audio]

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:
  level: DEBUG
  logs:
    rtsp_audio: DEBUG

api:

ota:
  - platform: esphome

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  power_save_mode: none

rtsp_audio:
  bclk_pin: GPIO26
  lrclk_pin: GPIO25
  din_pin: GPIO27

  adc_type: external
  pdm: false

  port: 8554
  sample_rate: 16000
  bits_per_sample: 32bit
  channel: left

  i2s_port: 0
  use_stereo_slot: false
  sample_shift: 14
  gain: 4.0
  use_apll: false

  rtp_payload_type: 96
  packet_ms: 20
  debug: true
  status_interval: 10s
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

## ESPHome-like pin aliases

The short names are recommended:

```yaml
bclk_pin: GPIO26
lrclk_pin: GPIO25
din_pin: GPIO27
```

The component also accepts ESPHome-like aliases:

```yaml
i2s_bclk_pin: GPIO26
i2s_lrclk_pin: GPIO25
i2s_din_pin: GPIO27
```

Use one style or the other, not both.

## Configuration reference

| Option | Default | Notes |
|---|---:|---|
| `bclk_pin` / `i2s_bclk_pin` | required | I2S BCLK / SCK |
| `lrclk_pin` / `i2s_lrclk_pin` | required | I2S LRCLK / WS |
| `din_pin` / `i2s_din_pin` | required | I2S data input from microphone |
| `adc_type` | `external` | Only `external` is supported |
| `pdm` | `false` | PDM is not supported |
| `port` | `8554` | RTSP TCP control port |
| `sample_rate` | `16000` | RTP clock and I2S sample rate |
| `bits_per_sample` | `32bit` | Input I2S sample width. `32bit` is typical for INMP441 |
| `channel` | `left` | Use `right` if your INMP441 L/R pin is wired for right channel |
| `i2s_port` | `0` | ESP32 I2S controller: `0` or `1` |
| `use_stereo_slot` | `false` | Keep `false` for the tested INMP441 setup |
| `sample_shift` | `14` | Right shift applied to 32-bit raw samples before gain |
| `gain` | `1.0` | Software gain after shifting |
| `use_apll` | `false` | Optional APLL clock source |
| `rtp_payload_type` | `96` | Dynamic RTP payload type for L16 |
| `packet_ms` | `20` | RTP packet duration |
| `debug` | `false` | Enables periodic stats |
| `status_interval` | `10s` | Debug status interval |

## Audio tuning

Start with:

```yaml
use_stereo_slot: false
sample_shift: 14
gain: 4.0
packet_ms: 20
```

If audio is too quiet:

```yaml
sample_shift: 12
gain: 2.0
```

If audio is distorted, robotic, or chopped and the logs show clipping:

```yaml
sample_shift: 14
gain: 2.0
```

Avoid very aggressive settings such as:

```yaml
sample_shift: 10
gain: 2.0
```

That can clip heavily with INMP441 and sound robotic.

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

## i2s_port

ESP32 has two I2S hardware controllers: I2S0 and I2S1.

```yaml
i2s_port: 0
```

means I2S0. Use I2S1 only if another component is already using I2S0.

## Notes for Frigate/go2rtc

This component exposes an audio-only RTSP stream. Some consumers handle audio-only RTSP better than others. If direct Frigate use is unreliable, put `go2rtc` in between and let Frigate consume the go2rtc stream.

## Source and implementation notes

The first working prototype used Phil Schatzmann's Arduino AudioTools RTSP example as a reference point for proving that audio-only RTSP could be useful on this hardware:

- Repository: `pschatzmann/arduino-audio-tools`
- Example: `examples/examples-communication/rtsp/communication-audiokit-rtsp/communication-audiokit-rtsp.ino`

This native ESP-IDF component does **not** copy or link Arduino AudioTools. It reimplements the minimal pieces needed for this ESPHome use case:

- ESP-IDF I2S STD microphone input via `esp_driver_i2s` / `driver/i2s_std.h`
- TCP socket server for RTSP control
- RTSP methods: `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`, `TEARDOWN`
- SDP generation for `L16/<sample_rate>/1`
- UDP sockets for RTP
- RTP header creation, sequence numbers, timestamps, and SSRC
- 32-bit I2S sample conversion to signed 16-bit big-endian PCM for RTP L16

The implementation is intentionally compact and practical rather than a complete RTSP stack.

## License

No license is declared yet. Add a license before publishing publicly if you want others to reuse or modify the code under clear terms.
