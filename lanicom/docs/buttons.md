# Buttons, pairing and LED patterns

How the illuminated buttons on a wall box get linked to other wall boxes, and what their
LED rings show. Designed 2026-10-07, built the same day:

- protocol: [PROTOCOL.md](../spec/PROTOCOL.md) section 7 (v1.1);
- logic: `lanicom-core` `lc_buttons` (host-tested in `test/test_engine.c`);
- device: the `buttons:` option of the ESPHome component, set up in
  [`lanicom-p4.yaml`](../esphome/lanicom-p4.yaml).

Not built yet: night brightness, the boot sweep, and a brightness per button (there is one
"Ring brightness" for all of them).

## Decided

- **One button, one partner.** A link joins exactly two buttons, on two devices.
- **Buttons are the only way to call.** A device can only talk to the partners on its
  buttons. There is no "all" button.
- **Group calls:** hold two, three or four buttons at the same time to talk to all of those
  partners at once.
  - The buttons don't have to go down together. A button pressed during a talk adds its
    partner to the running stream (with a `TalkStart`).
  - Releasing a button removes its partner again. The talk ends when the last button is
    released.
- **Devices with one button exist.** The house plan:
  - kitchen and workshop, 4 buttons each;
  - one device in each child's room, with 1 button.
  - A child can only call the partner on their one button (for example the kitchen).
- **Home Assistant announcements reach any device.** Pairing is about buttons only.
- **No reset or unpair gesture on the buttons.** Kids mash buttons, so every reset and
  unpair goes through the device's web page or Home Assistant (see Reset). Buttons do
  three things: talk, group talk, pair.
- **The network key is baked in at flash time**, and can still be changed on the web
  page. Friends get a device that just works and never need to see the page.
- **Configuration beyond pairing** goes through ESPHome entities. They appear on the
  device's built-in web page (`web_server`, already enabled) and in Home Assistant.

## Pieces it builds on

- **Device identity:** every device has a `sender_id`, saved at first boot
  ([PROTOCOL.md](../spec/PROTOCOL.md) §3). The ESPHome firmware derives it from the MAC
  address, so it survives a factory reset too. A link stores the partner's `sender_id`
  and button number, so it survives reboots and IP changes.
- **Talking:** a button talks with target `device:<partner sender_id>`, which the protocol
  already supports (§6, Addressing).
- **Presence:** every device sends a `Hello` every 5 s, and peers are forgotten after 30 s
  (§5).

## Pairing

A new device comes out of the box with nothing linked: its rings are off and its buttons
do nothing.

1. On device A, hold an **unlinked** button for 5 s. That button enters pairing mode: its
   ring blinks slowly, and A broadcasts a `PairOffer` for it once a second.
2. Within 60 s, on device B, hold an **unlinked** button for 5 s. It enters pairing mode
   too, sees A's offer and accepts it.
3. Both rings flash to confirm. The two buttons are linked.

- **Pairing mode** is per button. It ends on success, after 60 s, or when the button is
  pressed briefly.
- **A linked button never enters pairing mode.** On a linked button a long hold is just a
  long talk. That is why there is no conflict between talking and pairing.
- **If B sees more than one open offer** (two pairings going on in the house at once), it
  takes the oldest one.
- **Security:** all of this is ordinary CONTROL traffic, so only devices with the network
  key take part. The 5 s holds on both devices are the user's confirmation.

### Links heal themselves

Each device lists its links in its `Hello`. A device that hears a verified `Hello` from a
partner that doesn't list the link back drops its own side. That button becomes unlinked
and can be paired again.

So resetting one device unlinks its partners' buttons within seconds. A partner that was
offline at the time catches up as soon as both are online again. Nothing needs an explicit
"unlink" message.

If a partner never comes back (a broken device), its buttons keep pointing at it and show
"partner offline". Resetting the links on that device frees them.

### Protocol additions

In `lanicom.proto` and PROTOCOL.md (v1.1). Old devices skip unknown fields and messages,
so this is backwards compatible.

```proto
message PairOffer   { fixed64 nonce = 1; uint32 button = 2; }  // broadcast 1/s while a button is in pairing mode
message PairAccept  { fixed64 nonce = 1; uint32 button = 2; }  // unicast to the offerer
message PairConfirm { fixed64 nonce = 1; }                     // unicast back; both save the link

message Link { uint32 button = 1; fixed32 partner = 2; uint32 partner_button = 3; }
// Hello: repeated Link links = 11;  Control: pair_offer = 11, pair_accept = 12, pair_confirm = 13
// caps: bit 3 ANNOUNCER (Home Assistant)
```

If two buttons enter pairing mode within the same second, both offer; the one with the
larger nonce accepts the other's offer, so exactly one link is made.

## Reset and unpair

No button gesture: a child pressing a glowing button five times, or holding it, must not
be able to unlink the kitchen.

| How | What it clears |
|---|---|
| "Unpair" next to a button, on the web page or in Home Assistant | That button's link |
| "Factory reset" on the web page or in Home Assistant (ESPHome's `factory_reset` button) | Everything saved at runtime: links, brightness, and a network key or name changed on the page. Baked-in values (the key, the name) come back. |
| Power-cycle the device 5 times within 10 s (ESPHome's `factory_reset: resets_required: 5`) | The same, for when the network side is broken. On PoE: toggle the switch port, or the cable. The rings flash on each count (`on_increment`). Five deliberate cycles in 10 s don't happen by accident, not even with a flaky switch or a flickering power cut. |

The partner's side of a removed link clears itself (see "Links heal themselves").

## Receiving and Home Assistant

A device plays audio from the partners on its buttons, and from any peer that has the new
`ANNOUNCER` capability bit (`caps` bit 3) in its `Hello`. That is Home Assistant. A stream
from anyone else is dropped. Only devices with the network key can claim the bit.

Home Assistant addresses devices the way a group call works: it picks the set of recipients
and sends each one a copy (unicast fan-out, as now). That can be one device, any list of
devices, or all of them. Groupings such as "upstairs" are Home Assistant areas or labels,
not something the devices know about.

## LED patterns

Each ring belongs to one button. Brightness is set by PWM through the carrier's
transistors. The patterns differ in rhythm, not only in brightness, so a 1-button device
can show every state on its one ring.

### Per button

| State | Pattern | Meaning |
|---|---|---|
| Unlinked | Off | No partner |
| Linked, partner online | Steady dim (~10 %) | Ready to talk |
| Linked, partner offline | Dim, with a short dip every 3 s | Partner not heard from for 30 s |
| Talking | Full on | Held; audio is going out |
| Receiving | Breathing (≈1 Hz, dim ↔ full) | The partner on this button is talking to you |
| Pairing mode | Slow blink (1 Hz) | Waiting for another button to pair with |
| Linked (just now) | 3 quick flashes, then steady dim | Pairing succeeded |
| Error | Fast flicker (8 Hz) for 1 s, then back | Short press on an unlinked button, or pairing timed out |

### Whole device (all rings together)

These override the per-button states.

| State | Pattern | Meaning |
|---|---|---|
| Booting | One fade up and down (on 4 buttons: a sweep 1 → 4). Not built yet | Starting up |
| Announcement | All rings breathe together | Home Assistant is talking to this device |
| Reset count | One flash per power cycle | Counting towards a factory reset |
| Identify | All rings flash for 10 s | "Identify" pressed on the web page |
| No network | Short blink every 2 s (on 4 buttons: a running light) | No Ethernet link or no IP |
| Not provisioned | Triple blink, repeating | No network key set |

The original plan had four states: idle (dim), talking (on), receiving (pulse) and not
provisioned (blink). They map onto "linked, partner online", "talking", "receiving" and
"not provisioned" above.

## Web page (ESPHome entities)

ESPHome's `web_server` (version 3, password-protected) is already enabled. It shows every
entity automatically, and the same entities appear in Home Assistant. Nothing here needs
custom web code.

- **Per button:**
  - its partner (device name and button) and whether it is online;
  - "Unpair";
  - the ring's idle brightness (built as one "Ring brightness" for all rings).
- **Device:**
  - name;
  - network key (defaults to the baked-in one);
  - speaker volume and mic gain (already there);
  - night brightness for the rings, and the hours it applies (needs the time from Home
    Assistant or SNTP). Not built yet;
  - "Identify": all rings flash for 10 s, to find which box this is;
  - "Restart" (already there) and "Factory reset".
- **Status (read-only):** IP address, peers, dropped packets (already there), firmware
  version, uptime.
- **Firmware update:** a file upload on the page (ESPHome `ota: platform: web_server`), next
  to the network OTA. Once the P4 is in the box, USB is out of reach.
