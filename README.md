# Intercom boerderij

A home intercom built from Waveshare ESP32-P4-WIFI6-POE-ETH wall boxes. There are two routes:

- **Mumble:** the boxes run a Mumble client. There is one Mumble server for the house,
  Mumble channels route audio to rooms, and Home Assistant is integrated.
- **lanicom** (`lanicom/`): serverless. Boxes find each other on the LAN and talk
  directly, with encrypted unicast. No Mumble server, and Home Assistant is optional (it can join as a
  peer), so friends can build it too. See [lanicom/README.md](lanicom/README.md) and the
  plan with its design review in [lanicom/PLAN.md](lanicom/PLAN.md).

| Path | What |
| --- | --- |
| `lanicom/` | Serverless intercom: protocol spec, C core, ESPHome component, Python CLI, HA integration |
| `esp32-mumble/` | [dchote/esp32-mumble](https://github.com/dchote/esp32-mumble) (git subtree) with ESP32-P4 support and audio fixes. Board config: `esp32-mumble/esphome/waveshare-esp32-p4-poe.yaml` |
| `esphome/p4-bringup.yaml` | Minimal bring-up config for the board (Ethernet, codec, mic level) |
| `server/mumble/` | Docker Compose file for the Mumble server (Murmur) |
| `tools/voicetest/` | End-to-end voice test: play speech near the board, record it from Mumble, score it with Whisper |
| `PROJECT_NOTES.md` | Decisions, measurements and open items |

## Build and flash

```sh
python3 -m venv .venv && .venv/bin/pip install esphome==2026.7.1
cd esp32-mumble/esphome
../../.venv/bin/esphome run waveshare-esp32-p4-poe.yaml   # first flash over USB-C, later OTA
```

The Mumble server address is filled in automatically with Home Assistant's IP when
the device is adopted, or set it under the device's "1. Server" setting.

## Voice test

```sh
tools/voicetest/run.sh local ptt tools/voicetest/berend-nl-pad.wav nl
```

Plays the clip on this machine's default audio output, records what the board sends
to the Mumble server, and prints Whisper's transcript and word error rate. See the
comments in `run.sh` for the options.
