# Buttons, pairing and LED patterns (design, not built yet)

How the four illuminated buttons on a wall box get linked to other wall boxes, and what
their LED rings show. Captured 2026-10-07. Nothing here is implemented; the firmware still
has one PTT button with a fixed target.

## The idea

A new device comes out of the box with nothing linked: all four rings are off and the
buttons do nothing. You link two buttons, on two devices, by long-pressing both:

1. On device A, hold a button for 5 s. Its ring blinks slowly: it is in pairing mode.
2. Walk to device B and hold a button there for 5 s.
3. Both rings flash to confirm. The two buttons are now linked: pressing A's button talks
   to B, and pressing B's button talks to A. Each ring now shows the other device's state.

No app, no web page, no Home Assistant needed. The two devices only need to be on the same
network with the same network key (see [friends.md](friends.md)).

## Pieces it builds on

- **Device identity:** every device has a `sender_id`, random at first boot and then saved
  ([PROTOCOL.md](../spec/PROTOCOL.md) §3). A link stores the partner's `sender_id` and its
  button number, so it survives reboots and IP changes.
- **Talking:** a linked button talks with target `device:<sender_id>`, which the protocol
  already supports (§6, Addressing).
- **Network:** each device gets its IP by DHCP over Ethernet and is reachable as
  `<name>.local`. ESPHome's `web_server` is already enabled.

## Behaviour

### Per button

A button is either **unlinked** or **linked** to (partner `sender_id`, partner button).
The links are saved in flash.

| Action | Unlinked button | Linked button |
|---|---|---|
| Short press / hold | Nothing (error flash) | Talk to the partner while held |
| Hold 5 s | Pairing mode | Talks the whole time; no pairing (see "Holding while talking") |
| Hold it plus any other button, 5 s | Pairing mode | Pairing mode (re-link) |
| Hold it plus any other button, 10 s | — | Unlink |

**Holding while talking:** on a linked button, a plain long press has to stay a long
talk, or someone who talks for 5 s would end up pairing. So on a linked button only the
two-button chord starts pairing. On an unlinked button the plain 5 s hold is safe, because
there is nothing to talk to.

### Pairing

- **Pairing mode** lasts 60 s, or until it succeeds or you short-press the button to cancel.
- **While in pairing mode** a device broadcasts a `PairOffer` (its button number, a random
  nonce) every second.
- **The second device to enter pairing mode** sees the first one's offer. It answers with a
  `PairAccept` to the first device, unicast (the offer's nonce, its own button number).
- **The first device** checks the nonce, saves the link and replies `PairConfirm`. The
  second device saves the link when that arrives. Both flash "linked".
- **Two devices entering pairing mode at the same moment:** the one with the lower
  `sender_id` accepts.
- **Three or more devices in pairing mode at once:** the accepter can't tell which offer
  is meant. It does nothing and flashes the error pattern; try again.
- **Re-linking or unlinking a button** sends an `Unlink` to the old partner, which clears
  its side too. If the old partner is offline, it keeps a stale link. It shows that
  partner as offline until the button is re-linked or unlinked there.
- **Security:** all of this is ordinary CONTROL traffic, so only devices with the network
  key can pair. The 5 s hold on both devices is the user's confirmation.

### Protocol additions (draft)

Three new `Control` messages, to be added to `lanicom.proto` and PROTOCOL.md as v1.1.
Old devices skip unknown fields, so this is backwards compatible.

```proto
message PairOffer  { uint32 button = 1; fixed64 nonce = 2; }     // broadcast, 1/s while pairing
message PairAccept { fixed64 nonce = 1; uint32 button = 2; }     // unicast to the offerer
message PairConfirm{ fixed64 nonce = 1; }                        // unicast back
message Unlink     { uint32 button = 1; }                        // "forget your link to my button N"
```

## LED patterns

Each ring belongs to one button. Brightness is set by PWM through the carrier's transistors.
The patterns are chosen to be easy to tell apart at a glance and in peripheral vision.

### Per-button states

| State | Pattern | Meaning |
|---|---|---|
| Unlinked | Off | This button isn't linked to anything |
| Linked, partner online | Steady dim (~10 %) | Ready to talk |
| Linked, partner offline | Dim, with a short dip every 3 s | Partner hasn't been heard from for 30 s |
| Talking | Full on | You're holding the button and audio is going out |
| Receiving | Breathing (≈1 Hz, dim ↔ full) | The partner on this button is talking to you |
| Pairing mode | Slow blink (1 Hz, 50 %) | Waiting for the other device |
| Linked (just now) | 3 quick flashes, then steady dim | Pairing succeeded |
| Error | Fast flicker (8 Hz) for 1 s, then back to the previous state | Pressed an unlinked button, pairing timed out, or more than one offer was seen |

### Whole-device states (all four rings together)

These override the per-button states.

| State | Pattern | Meaning |
|---|---|---|
| Booting | One sweep, 1 → 4 | Starting up |
| No network | Running light, 1 → 4, repeating | No Ethernet link or no IP |
| Not provisioned | All rings slow blink in unison | No network key set (the original plan's "not provisioned") |
| Incoming call, unlinked sender | All rings breathe together | Someone talks to this device who isn't on any of its buttons (a zone or `all` call, or Home Assistant) |

The original plan had four states: idle (dim), talking (on), receiving (pulse) and not
provisioned (blink). They map onto "linked, partner online", "talking", "receiving" and
"not provisioned" above.

## Later: configuration page

Pairing by button covers the common case. A page on the device itself would cover the rest:

- **Per button:**
  - see and change the link (a list of the devices on the network, or a zone, or "all");
  - clear the link;
  - set the ring's idle brightness.
- **Device:** name, zones, and an LED brightness for day and night.

The cheapest way is to expose these as ESPHome entities (`select`, `number`, `button`).
They then appear automatically on the built-in `web_server` page and in Home Assistant.
A custom page (ESPHome `web_server` version 3 with its own JS) can come later, if the
built-in one is too plain.

## Open questions

- Should the button numbering in links survive swapping a P4 between carriers? (The
  `sender_id` lives on the P4, so a swapped P4 takes its links with it.)
- Should a linked button that is held while its partner is offline still try (maybe the
  partner just rebooted), or refuse with the error flash?
- Is one link per button enough, or should a button be able to call a zone, for example
  "upstairs"? The config page could allow it; pairing by button would stay one-to-one.
