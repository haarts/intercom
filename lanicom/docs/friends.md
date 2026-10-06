# Building a lanicom intercom

This sets up wall intercoms that talk to each other over your home network.
You don't need a server or Home Assistant. Every device on one intercom
network shares a secret network key, and devices without it can't listen
in or talk.

## What you need per device

| Part | Notes |
|---|---|
| Waveshare **ESP32-P4-WIFI6-POE-ETH** | Has the codec and mic on board. Powered by PoE or USB-C. |
| Speaker, 8 Ω 2 W, with an MX1.25 plug | "JST 1.25" hobby cables are usually PicoBlade-compatible; JST GH is not. |
| Momentary push button | Wired between GPIO22 and GND. Never connect a pin to 5 V. |
| Ethernet cable (recommended) or Wi-Fi | Wired gives the lowest latency. |

## 1. Make a network key (once)

On any computer with Python:

```sh
pip install 'lanicom[qr] @ git+https://github.com/haarts/intercom#subdirectory=lanicom/python'
lanicom keygen --qr
```

Keep the key (`abcde-fghij-...`) somewhere safe. Every device on this
intercom needs exactly this key.

## 2. Flash a device

```sh
python3 -m venv .venv && .venv/bin/pip install esphome==2026.7.1
cd lanicom/esphome
cp secrets.example.yaml secrets.yaml             # then fill in the passwords and the API key
../../.venv/bin/esphome run lanicom-p4.yaml      # first time over USB-C, later over the network
```

Don't skip the passwords. Someone on your network who could change the key and
switch on "Talk" could listen in on the room.

To bake the key in, add `-s network_key 'abcde-fghij-...'` after `esphome`.
To set what the button talks to, add `-s ptt_target zone:kids`.

## 3. Set the key

1. Plug the device into the network.
2. Open `http://lanicom-p4-xxxxxx.local/` and log in as `admin` with your `web_password`. The suffix is printed in the flash
   log; your router's device list also shows the device.
3. Paste the key into **Network key**.
4. Optionally set **Intercom name** (for example "Kitchen") and **Intercom
   zones** (for example `downstairs,kitchen`).

Within a few seconds, **Intercom peers** shows the other devices. Hold the
button to talk.

## 4. Optional: Home Assistant

- To manage the board from HA, adopt it like any ESPHome device.
- To make HA a peer, copy `lanicom/ha/custom_components/lanicom` into HA's
  `config/custom_components/`, restart HA, and add the **lanicom intercom**
  integration with the same key. HA then gets a "Talking" sensor per device,
  the events `lanicom_talk_start` / `lanicom_talk_stop`, and the
  `lanicom.announce` action:

```yaml
action: lanicom.announce
data:
  target: zone:kids          # all | zone:<name> | device:<id> | a device name
  media: media-source://tts/tts.google_translate_en_com?message=Dinner%20is%20ready
```

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| **Intercom peers** stays 0 | The key differs between devices, or broadcast doesn't reach (see the next rows). The "Dropped packets" sensor counts traffic from other keys. |
| Works on cable, not on Wi-Fi | The access point's **client isolation** (or "guest network") blocks traffic between devices. Turn it off for this network. |
| Devices on different VLANs/subnets don't see each other | Broadcast doesn't cross routers. List the other devices' IPs as `static_peers:` in the YAML (both directions), and allow UDP 47100 between the VLANs. |
| `multicast: true` is choppy on Wi-Fi | Expected: Wi-Fi sends multicast slowly and without retries. Use the default (unicast). |
| Audio is choppy | Raise `jitter_buffer` (e.g. 40ms), or use a cable. Check that nothing else saturates the Wi-Fi. |
| Delay feels long | Try the low-latency audio option in `lanicom-p4.yaml` (`tools/make_lowlatency_i2s.py`). |
| First syllable cut off | Press the button, then start talking. The mic needs about 50 ms to start. |

`lanicom list` on a laptop shows every device it can reach, with its round-trip time.
