# Buttons, pairing and LED patterns (design, not built yet)

How the illuminated buttons on a wall box get linked to other wall boxes, and what their
LED rings show. Captured 2026-10-07. Nothing here is implemented; the firmware still has
one PTT button with a fixed target.

## Decided

- **One button, one partner.** A link joins exactly two buttons, on two devices.
- **Buttons are the only way to call.** A device can only talk to the partners on its
  buttons. There are no zones and no "all" in the user interface.
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
- **No unpairing per button.** To change links, reset the device's links and pair its
  buttons again. Re-pairing four buttons to fix one is rare enough to accept.
- **Configuration beyond pairing** goes through ESPHome entities. They appear on the
  device's built-in web page (`web_server`, already enabled) and in Home Assistant.

## Pieces it builds on

- **Device identity:** every device has a `sender_id`, random at first boot and then saved
  ([PROTOCOL.md](../spec/PROTOCOL.md) §3). A link stores the partner's `sender_id` and
  button number, so it survives reboots and IP changes.
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

### Protocol additions (draft)

To be added to `lanicom.proto` and PROTOCOL.md as v1.1. Old devices skip unknown fields and
messages, so this is backwards compatible.

```proto
message PairOffer   { fixed64 nonce = 1; uint32 button = 2; }  // broadcast 1/s while a button is in pairing mode
message PairAccept  { fixed64 nonce = 1; uint32 button = 2; }  // unicast to the offerer
message PairConfirm { fixed64 nonce = 1; }                     // unicast back; both save the link

message Link { uint32 button = 1; fixed32 partner = 2; uint32 partner_button = 3; }
// Hello gets: repeated Link links = 7;
```

## Reset

| How | What it clears | When |
|---|---|---|
| Hold any button for 30 s | All links (the device keeps its name, network key and identity) | Normal way; works on 1-button devices with the box closed |
| "Reset links" on the web page or in Home Assistant | The same | When you're at a computer anyway |
| "Factory reset" on the web page or in Home Assistant (ESPHome's standard one) | Everything: links, name, network key | Giving a device away, or starting over |

**The 30 s hold:**
- **0 to 20 s:** normal talk if the button is linked. If it isn't, the ring enters pairing
  mode at 5 s.
- **From 20 s:** the talk stops (or pairing is cancelled), and all rings flicker as a
  countdown. Let go to cancel.
- **At 30 s:** links cleared. All rings flash, then go off.

A talk longer than 20 s gets cut, which is fine for an intercom.

## Receiving

A device plays audio from the partners on its buttons, and Home Assistant announcements.
A stream from anyone else is dropped.

Telling an announcement apart needs a way to recognise Home Assistant: a capability bit
in its `Hello`, still to be defined.

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
| Booting | One fade up and down (on 4 buttons: a sweep 1 → 4) | Starting up |
| Announcement | All rings breathe together | Home Assistant is talking to this device |
| Reset countdown | Fast flicker, getting faster | A button has been held for 20 s; let go to cancel |
| Links reset | Two long flashes, then off | Done |
| No network | Short blink every 2 s (on 4 buttons: a running light) | No Ethernet link or no IP |
| Not provisioned | Triple blink, repeating | No network key set |

The original plan had four states: idle (dim), talking (on), receiving (pulse) and not
provisioned (blink). They map onto "linked, partner online", "talking", "receiving" and
"not provisioned" above.

## Configuration (ESPHome entities)

These show up on the device's own web page and in Home Assistant:

- **Per button:**
  - its partner (device name and button, read-only);
  - the ring's idle brightness.
- **Device:**
  - its name;
  - one brightness for all rings, maybe with a night setting;
  - "Reset links" and "Factory reset" buttons.

## Open

- Is 30 s right for the reset hold, and 20 s for the talk cut-off?
- How Home Assistant identifies itself so devices accept its announcements (a capability bit).
