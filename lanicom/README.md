# lanicom: serverless LAN intercom

Push-to-talk intercom devices that find each other on the LAN and talk
directly. No Mumble server, and Home Assistant is optional (it can join as a
peer). Every packet is encrypted and authenticated with a shared network key.

- Why it's built this way, the status and the latency budget: [PLAN.md](PLAN.md)
- The wire protocol: [spec/PROTOCOL.md](spec/PROTOCOL.md)
- Setting up devices (for friends): [docs/friends.md](docs/friends.md)

| Path | What |
|---|---|
| `spec/` | Protocol, protobuf schema, shared test vectors |
| `lanicom-core/` | Portable C protocol engine (used by the firmware), with host tests |
| `esphome/` | ESPHome component and the Waveshare ESP32-P4 board config |
| `python/` | Reference implementation and the `lanicom` command-line peer |
| `ha/` | Home Assistant integration |

## Try it on two computers

```sh
pip install -e 'lanicom/python[audio]'   # needs libopus0 and PortAudio (Debian: apt install libopus0 libportaudio2)
lanicom keygen --save                    # once; copy ~/.config/lanicom/key to the other machine
lanicom list                             # who is on the network
lanicom talk --to all                    # Enter starts/stops talking; also plays what others say
lanicom send doorbell.mp3 --to zone:kids # play a file (needs ffmpeg)
lanicom record out.wav --duration 10     # record what others say
```

## Tests

```sh
cmake -S lanicom/lanicom-core -B lanicom/lanicom-core/build && cmake --build lanicom/lanicom-core/build \
  && ctest --test-dir lanicom/lanicom-core/build          # C core (needs libmbedtls-dev), ASan/UBSan
pip install -e 'lanicom/python[test]' && pytest lanicom/python   # Python + C interop (after the C build)
cd lanicom/ha && pytest                                   # HA (pip install pytest-homeassistant-custom-component ha-ffmpeg)
```

After changing the protocol, regenerate the vectors (`python lanicom/python/tools/gen_vectors.py`)
and the integration's copy of the library (`python lanicom/ha/sync_lib.py`).
