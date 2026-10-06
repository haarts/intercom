# Buttons, pairing and LED patterns (design, not built yet)

How the illuminated buttons on a wall box get linked to other wall boxes, and what their
LED rings show. Captured 2026-10-07. Nothing here is implemented; the firmware still has
one PTT button with a fixed target.

## Decided

- **Buttons are the only way to call.** A device can only talk to devices that are on one
  of its buttons. There are no zones and no "all" in the user interface.
- **Group calls:** hold two, three or four buttons at the same time to talk to all of
  those partners at once.
  - The buttons don't have to go down together. A button pressed during a talk adds its
    partners to the running stream (they get a `TalkStart`).
  - Letting go of a button removes its partners again. The talk ends when the last button
    is released.
- **Devices with one button exist.** The house plan:
  - kitchen and workshop, 4 buttons each;
  - one device in each child's room, with 1 button.
- **Configuration beyond pairing** goes through ESPHome entities. They appear on the
  device's built-in web page (`web_server`, already enabled) and in Home Assistant. There
  is no custom page for now.
- **Whole-device states** (booting, no network, ...) use all rings together.

## Pieces it builds on

- **Device identity:** every device has a `sender_id`, random at first boot and then saved
  ([PROTOCOL.md](../spec/PROTOCOL.md) §3). A link stores the partner's `sender_id`, so it
  survives reboots and IP changes.
- **Talking:** a button talks with target `device:<sender_id>` for each of its partners.
  The protocol already supports this (§6, Addressing).
- **Network:** each device gets its IP by DHCP over Ethernet and is reachable as `<name>.local`.

## Pairing (open: leading proposal)

A new device comes out of the box with nothing linked: its rings are off and its buttons
do nothing.

### The problem

The first idea was to hold a button for 5 s on both devices. It fails in two ways:

1. **A linked button can't use a long hold to pair.** On a linked button, a long hold is a
   long talk, so anyone who talks for 5 s would start pairing. A separate gesture for
   linked buttons (a two-button chord) means two ways to pair, which is confusing.
2. **A 1-button device can only pair once.** After that its button is linked, and a chord
   is impossible with one button. Yet in the house plan, each child's button should reach
   both the kitchen and the workshop.

### Proposal: only the device that starts needs a free button

1. On device A, hold an **unlinked** button for 5 s. A broadcasts a `PairOffer`. That
   button's ring blinks slowly.
2. **Every other device** that hears the offer blinks all its rings together: "someone
   wants to pair". While they blink, pressing a button doesn't talk; it accepts.
3. On device B, press the button to link, whether it's linked already or not. Both devices
   flash to confirm:
   - A's button now calls B;
   - B's button now calls A, as well as any partners it already had.
4. All other devices stop blinking. The window closes after the first accept, or after 60 s.

For the house plan:
- the kitchen and the workshop each start a pairing from a free button;
- in each child's room you press the one button to accept;
- the child's button ends up calling both the kitchen and the workshop.

**Trade-off:** for up to 60 s, every device in the house blinks and turns its next press
into an accept. That's visible, which is good, but a child pressing their button at that
moment gets linked by accident. It shows, though, and you remove it on the config page.

**Removing a link:** on the config page (per button: its partners, with "remove").
The partner is told with an `Unlink`, so its side goes too. If the partner is offline, it
keeps a stale link until it comes back and is told.

**Still open:**
- Is the house-wide blink acceptable, or should accepting need a longer hold on the
  responder (for example 2 s)? A longer hold costs nothing in talking, because accept mode
  isn't talk mode.
- Should a button have a maximum number of partners?

### Protocol additions (draft)

To be added to `lanicom.proto` and PROTOCOL.md as v1.1. Old devices skip unknown messages,
so this is backwards compatible.

```proto
message PairOffer   { fixed64 nonce = 1; uint32 button = 2; bool cancel = 3; } // broadcast 1/s while open
message PairAccept  { fixed64 nonce = 1; uint32 button = 2; }                   // unicast to the offerer
message PairConfirm { fixed64 nonce = 1; }                                      // unicast back; closes the window
message Unlink      { uint32 button = 1; }  // "remove me from the button that links to my button N"
```

- **Who closes the window:** the offerer sends one last `PairOffer` with `cancel = true` after
  confirming, after a timeout, or when it is cancelled. That stops the blinking everywhere.
- **Two offers at once:** a device that hears offers from two devices shows the error
  pattern and accepts neither.
- **Security:** all of this is ordinary CONTROL traffic, so only devices with the network
  key take part. The physical presses on both devices are the user's confirmation.

### Receiving

A device plays audio only from its partners. A stream from anyone else is dropped.
Whether Home Assistant announcements are the one exception is still open.

## LED patterns

Each ring belongs to one button. Brightness is set by PWM through the carrier's
transistors. The patterns use different rhythms, not just different brightness. That way a
1-button device can show every state on its one ring.

### Per button

| State | Pattern | Meaning |
|---|---|---|
| Unlinked | Off | No partners |
| Linked, partners online | Steady dim (~10 %) | Ready to talk |
| Linked, a partner offline | Dim, with a short dip every 3 s | A partner hasn't been heard from for 30 s |
| Talking | Full on | Held; audio is going out |
| Receiving | Breathing (≈1 Hz, dim ↔ full) | A partner on this button is talking to you |
| Offering to pair | Slow blink (1 Hz) | This button started a pairing; waiting |
| Linked (just now) | 3 quick flashes, then steady dim | Pairing succeeded |
| Error | Fast flicker (8 Hz) for 1 s, then back | Pressed an unlinked button, pairing timed out, or two offers were seen |

### Whole device (all rings together)

These override the per-button states.

| State | Pattern | Meaning |
|---|---|---|
| Booting | One fade up and down (on 4 buttons: a sweep 1 → 4) | Starting up |
| Someone wants to pair | Double blink, repeating | Press a button to accept |
| No network | Short blink every 2 s (on 4 buttons: a running light) | No Ethernet link or no IP |
| Not provisioned | Triple blink, repeating | No network key set |

The original plan had four states: idle (dim), talking (on), receiving (pulse) and not
provisioned (blink). They map onto "linked, partners online", "talking", "receiving" and
"not provisioned" above.

## Configuration (ESPHome entities)

These show up on the device's own web page and in Home Assistant:

- **Per button:**
  - its partners (by device name), each with a remove action;
  - the ring's idle brightness.
- **Device:**
  - its name;
  - one brightness for all rings, maybe with a night setting.
