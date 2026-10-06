# Intercom boerderij — project notes

Summary of the claude.ai project "Intercom boerderij"
(https://claude.ai/project/01a0be9b-925f-714b-8b62-d522bc2748fb) and the Claude Design
project "Art Deco Intercom Grill"
(https://claude.ai/design/p/af2fbe00-a71f-4dd4-8ccf-62192279e134), pulled 2026-10-01.

## Goal

A home intercom that is **near-instant** (hard requirement) and integrates with
**Home Assistant** (strongly wanted: listen to a room, use the speakers from HA).
Form factor: a wall-mounted box with a brass front plate. The plate has **four room
buttons** (3 children's rooms, 1 workshop), each with an LED ring.

## How the design evolved

1. **Nov 2025: SIP/paging route (abandoned).** FreePBX/Asterisk, an ESP32 button panel
   and Grandstream PoE paging speakers (GSC3506).
2. **Sep 2026: ESP32 + Mumble route (current).** ESP32-P4 boxes running a Mumble
   client, one Mumble server for the house, with Mumble channels used to route to rooms.
3. **Oct 2026: serverless route, lanicom (in progress).** No server, no HA dependency, so
   friends can use it. Boxes discover each other by authenticated broadcast and send Opus
   by encrypted unicast. Same board and ESPHome stack. See `lanicom/PLAN.md`.

## Decisions made

| Area | Decision |
| --- | --- |
| Board | **Waveshare ESP32-P4-ETH** (sku ESP32-P4-POE-ETH, arrived 2026-10-01). Ethernet, PoE via a module that sits on top of the P4, 32MB PSRAM, onboard mic, codec, speaker header. No Wi-Fi. |
| Speaker | Onboard amp header (8Ω, 2W). Fine as is. |
| Mumble server | Official **Murmur** container (`mumblevoip/mumble-server`). go-mumble-server (same author as the client) is the alternative. |
| Firmware | **ESPHome**, using dchote/esp32-mumble as an external component, ported to the P4. Not plain ESP-IDF, and no MQTT (see below). |
| Buttons | Illuminated panel-mount momentary switch: **M19 thread, Φ22 bezel**, 36mm long, LED ring. Four of them. |
| Front plate | 180 × 90 mm, **3mm brass**, R3 corners, laser-cut by laseropmaat.nl. Fan/arches/chevron art-deco grille above a row of 4 buttons along the bottom. 4 × Ø3 countersunk mounting holes, 6mm from the edges. |
| Enclosure | Wooden box made by hand router, in **two halves**: a back tray for the board, speaker and PoE, and a front piece with a plate rebate plus relief pockets so each switch nut can be tightened. Screwed together, not glued. |

## Firmware: why ESPHome and not ESP-IDF + MQTT

dchote/esp32-mumble (last commit 2026-07-23, actively maintained) **is already an ESPHome
external component** for the **ESP32-S3**. The Sep 20 chat was right. The Oct 1 chat was
wrong: the repo is not an old standalone ESP-IDF project for the original ESP32. The
"standalone ESP-IDF + MQTT" advice rested on that mistake.

- ESPHome talks to HA over its own native API. The component already exposes mode
  (Always On / PTT / Communicator), mute, volume, channel select, server settings,
  connection, ping and voice-activity diagnostics, plus OTA updates. MQTT would only
  rebuild that by hand.
- Four room buttons are easy in YAML: on press, set the channel and start talking;
  on release, stop.
- Board configs for the ESP-IDF framework (Box, Box-3, Voice PE) already use the same
  ES8311 DAC through ESPHome's `audio_dac` / `i2s_audio` components.

### What the P4 port actually involves (from reading the source)

1. **Opus:** `lib/micro-opus/library.json` hard-codes `-DOPUS_XTENSA_LX7` and
   `celt/xtensa/mathops_lx7.c`. For RISC-V, make those conditional and use the
   generic C path.
2. **Wi-Fi assumptions:** `mumble_component.cpp` calls `esp_wifi_set_ps()` and reads the
   MAC from `ESP_MAC_WIFI_STA`. Guard these for Ethernet (use the Ethernet MAC).
   `mumble_client.cpp` waits 5 s for "Wi-Fi stability". It's harmless, but the name
   is now misleading.
3. **Board YAML:** a new `esphome/waveshare-esp32-p4-poe.yaml` with `ethernet:` instead of
   `wifi:`, PSRAM, I2S pins, the ES8311 codec, and the 4 buttons and LEDs. The
   ESP-IDF networking path (lwIP netconn) is network-agnostic.
4. Check that **ESPHome supports the P4** well enough for Ethernet + I2S. The repo pins
   ESPHome 2026.7.1.
5. Worth opening a discussion or PR upstream, since the maintainer welcomes new hardware.

## Front plate DXF check (`~/Downloads/intercom-grill-arches.dxf`)

The two copies in Downloads are identical. The file is 180 × 90 mm, with layers `CUT` and
`INFO_CSK` (countersink note for the firm).

- **The button holes are Ø16** (r=8, at x = 42/74/106/138, y = 14). The M19 switches need
  **Ø19.3–19.5**. Fix this in the design's buttonDiameter tweak before ordering.
- With Φ22 bezels, a 32mm pitch leaves 10mm between bezels. The bezel's bottom edge
  sits 3mm from the plate edge, so check that the wooden frame doesn't overlap it.
- Mounting holes are Ø3.0. M3 screws need about Ø3.2–3.4.
- **Pattern vs. 3mm brass:** at 35mm repeat, only **Chevron** is comfortably cuttable
  (1.7mm slots, 2.1mm webs). Arches and Fan scallop need about 55mm repeat or thinner
  brass. The design is currently set to a 55mm fan width with a 2mm minimum web. Ask
  laseropmaat their minimum slot and web width for 3mm brass.

## Open items

- [ ] Set the button holes to Ø19.3–19.5 and re-export the DXF.
- [ ] Measure the switch's engageable thread length and nut size across flats (sets the land and pocket size).
- [ ] Ask laseropmaat about the minimum slot and web width; pick the pattern and repeat size.
- [ ] PoE infrastructure isn't there yet. Power over USB-C for now; Ethernet works without PoE.
- [x] Bring-up firmware (`esphome/p4-bringup.yaml`) flashed 2026-10-01. Chip rev v3.2, 32MB PSRAM, ES8311 up, Ethernet gets an IP (DHCP). Logs need `logger: hardware_uart: UART0` (USB-C is a CH343 bridge).
- [ ] Adopt in HA; test the speaker (Play Test Tone) and mic (Mic Peak/RMS). Wire a test button to GPIO20.
- [x] Port esp32-mumble to the P4 (2026-10-01): working copy in `esp32-mumble/`, board config
      `esp32-mumble/esphome/waveshare-esp32-p4-poe.yaml`. Builds and runs: Ethernet up, Opus
      encoder/decoder init, mic enabled. Uses ESPHome's **native esp-idf toolchain**, because
      PlatformIO's (pioarduino 55.03.39) can't link rev3 P4 builds (missing `sections.rev3.ld.in`).
      `lib/micro-opus/CMakeLists.txt` registers Opus as an IDF component for that toolchain.
- [x] Murmur 1.5 (official Docker image) runs on **meklit** (192.168.188.45:64738), compose file in `~/mumble-server`.
      The P4 connects: TLS, OCB2-AES128 crypto, UDP voice channel all confirmed (2026-10-01).
- [x] **Voice TX works end to end** (2026-10-01): board -> Murmur -> pymumble listener received audio.
      Mic fix: drop `use_microphone: true` from the es8311 config. In ESPHome's driver it selects a
      PDM *digital* mic (REG14 bit 6); this board's mic is analog, and the driver enables that without it.
- [x] WireGuard for remote access (2026-10-01): `wg1` on meklit (10.66.66.1/24, UDP 51820, enabled at boot),
      phone peer 10.66.66.2, limited to meklit only. Mumla server: 10.66.66.1:64738. meklit's existing
      `wg0` (an unrelated, disabled client config) left untouched.
- [x] Remote access works (2026-10-03): Mumla on the phone connects over WireGuard. Double NAT:
      Double NAT: the ISP router (192.168.2.254) forwards UDP 51820 -> FRITZ!Box (192.168.2.1), which
      forwards to meklit **192.168.188.45**. Watch out: the FRITZ!Box has a stale older device entry also
      named "meklit" (different IP/MAC); the sharing must use the current one. The endpoint in the phone
      config is the home's public IP, which may change: consider DynDNS.
- [ ] Speaker/RX test once a speaker is connected (MX1.25; "JST 1.25" hobby cables are usually PicoBlade-compatible, JST GH is not).
- [x] Voice quality (2026-10-03), measured with `tools/voicetest` (speech -> Bluetooth speaker in the closet ->
      P4 -> Mumble -> Whisper word error rate). Dutch test sentence: WER 9%, only the first word ("Hallo") lost.
      Fixes: VAD no longer raises its noise floor during speech, hangover 0.3 -> 0.8 s, push-to-talk bypasses
      VAD and its calibration, mic gain 72 dB -> 30 dB preamp + 24 dB digital (HA settings "Mic Preamp" /
      "Mic Digital Gain"). meklit's laptop speakers are useless as a test source (54% WER even on its own mic).
- [x] Choppy audio in Mumla at home was the tunnel, not the board: with WireGuard on over home Wi-Fi the traffic
      hairpins through the ISP router and loses ~65% of packets. Direct to 192.168.188.45: 0% loss.
      At home use the LAN address; WireGuard only when away.
- [x] **Mumla playback fixed** (2026-10-03): the board stepped the voice sequence by 1 per 20 ms packet and
      reset it to 0 each utterance. Mumble counts 10 ms frames (+2 per packet), and Mumla's jitter buffer
      dropped the mistimed packets. Fixed in `send_voice_packet()`. This is an upstream esp32-mumble bug that
      affects every board: worth reporting/PR. Mumla now "so much better"; over the tunnel at home slightly worse but usable.
- [x] First word (2026-10-03): 200 ms pre-roll in always_on, `mic_warmup: 50ms` (new option, default 200 ms).
      The lost "Hallo" in tests was the Bluetooth speaker muting on digital silence: test clips are now padded
      with soft noise. Dutch sentence: 0% WER in both modes.
- [x] Opus sweep (Harvard sentences, PTT): WER 4-7% at 16k/c1, 24k/c5 and 32k/c10 alike (run-to-run noise).
      Encode time per 20 ms frame: 2.4 / 5.5 / 8.8 ms. Kept 16 kbit/s, complexity 1 pending a listening A/B.
      Settings "Opus Bitrate" / "Opus Complexity" are live-adjustable in HA.
- [x] PTT button on GPIO22 (2026-10-03): button to GND, internal pull-up, active low. TX starts ~60 ms after the press.
      Never wire a GPIO to VBUS/VSYS (5 V): the P4 pins are 3.3 V only.
      2026-10-06: PTT moved to GPIO23, button 1 on the carrier board (buttons 1-4 = GPIO23/22/21/20 in header order).
      A breadboard button on GPIO22 must move to GPIO23 with the new firmware.
- [ ] Upstream PR https://github.com/dchote/esp32-mumble/pull/2 (branch `p4-support` on haarts/esp32-mumble,
      worktree /home/harm/prj/esp32-mumble-pr): P4 support, sequence fix, VAD. `esp32-mumble/` here is a git
      subtree identical to that branch; after merge, `git subtree pull --prefix=esp32-mumble <upstream> main`.
- [ ] lanicom (2026-10-06): spec, C core, Python CLI and HA integration done and tested off-device.
      Still to do on the board: flash `lanicom/esphome/lanicom-p4.yaml`, talk with `lanicom talk` on a laptop,
      and measure latency with and without `tools/make_lowlatency_i2s.py`. Stock ESPHome I2S queues about 50 ms
      on the speaker (5 × 10 ms DMA, preloaded), so expect about 120 ms mouth-to-ear on stock and about 70 ms
      with the override.
- [x] lanicom on the P4 (2026-10-06): flashed over the network (the old Mumble firmware's OTA had no password),
      found the laptop and verified it in seconds (RTT 2.3 ms over Wi-Fi). The PTT button on GPIO22 talks to the
      laptop's speakers, "perfectly". Voice test 0% WER. Click test, board sending half: ~34 ms stock, ~22-27 ms
      with the low-latency I2S override. The override is **not** used: first find out whether ~120 ms
      mouth-to-ear (estimated) is noticeable in half duplex. Fixed on the board: the speaker blocked the mic after
      playback. Laptop needs `ufw allow 47100/udp`. Next: a speaker on the board, then two boxes in different rooms.
- [ ] In HA: remove the old `intercom-p4-bringup` device, adopt `esp32-mumble-p4-e80332` (no API encryption yet).
