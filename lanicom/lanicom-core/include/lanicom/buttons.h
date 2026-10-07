/* Buttons: one partner per button, pairing, group talk and ring states (PROTOCOL.md, "Buttons").
 *
 * Sits on top of lc_engine: no clocks or I/O of its own. Feed it button presses, the engine's
 * hello/control callbacks and the time; read back which partners to talk to and what each ring
 * shows. Same locking as the engine (call it with the engine serialised).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LC_MAX_BUTTONS 4
#define LC_PAIR_HOLD_MS 5000          /* hold an unlinked button this long to pair it */
#define LC_PAIR_WINDOW_MS 60000       /* pairing mode ends after this */
#define LC_PAIR_OFFER_MS 1000         /* PairOffer interval */
#define LC_PAIR_OFFER_FRESH_MS 2500   /* an offer not repeated for this long is gone */
#define LC_PAIR_ACCEPT_RETRY_MS 500   /* PairAccept is repeated until confirmed... */
#define LC_PAIR_ACCEPT_GIVEUP_MS 5000 /* ...for this long */
#define LC_LINK_GRACE_MS 15000        /* a link this new isn't checked against the partner's Hello */
#define LC_FLASH_MS 900               /* "linked" flashes */
#define LC_ERROR_MS 1000              /* "error" flicker */

typedef enum {
  LC_LED_OFF,       /* unlinked */
  LC_LED_IDLE,      /* linked, partner online */
  LC_LED_OFFLINE,   /* linked, partner not heard from */
  LC_LED_TALKING,   /* held; audio going out */
  LC_LED_RECEIVING, /* the partner is talking to us */
  LC_LED_PAIRING,   /* waiting for another button */
  LC_LED_LINKED,    /* just paired */
  LC_LED_ERROR,     /* short press on an unlinked button, or pairing timed out */
} lc_led_state_t;

/* What a button is linked to; persist this. partner 0 means unlinked. */
typedef struct {
  uint32_t partner; /* sender_id */
  uint8_t partner_button; /* 1-based */
} lc_button_link_t;

typedef struct {
  lc_button_link_t link;
  uint32_t linked_ms;
  uint64_t link_nonce; /* of the pairing that made the link, to answer a repeated PairAccept */
  /* The physical button. */
  bool down, hold_done;
  uint32_t down_ms;
  /* Pairing mode: offering our own nonce, or accepting someone else's offer. */
  bool pairing;
  uint64_t nonce;
  uint32_t pairing_until_ms, next_offer_ms;
  uint32_t accept_peer; /* 0: not accepting */
  uint64_t accept_nonce;
  uint8_t accept_peer_button;
  uint32_t accept_next_ms, accept_until_ms;
  /* Ring feedback. */
  uint32_t flash_until_ms, error_until_ms;
} lc_button_t;

typedef struct {
  uint32_t sender;
  uint64_t nonce;
  uint8_t button;
  uint32_t first_ms, last_ms;
} lc_pair_offer_t;

typedef struct {
  lc_engine_t *engine;
  uint8_t n;
  lc_button_t b[LC_MAX_BUTTONS];
  lc_pair_offer_t offers[8]; /* other devices' open offers */
  bool links_changed;        /* a link was made or dropped: persist, then clear */
  bool talk_changed;         /* the set of partners to talk to changed */
} lc_buttons_t;

void lc_buttons_init(lc_buttons_t *bt, lc_engine_t *e, uint8_t n_buttons);
/* Restore persisted links (n entries, button order) and list them in our Hello. */
void lc_buttons_restore(lc_buttons_t *bt, const lc_button_link_t *links, size_t n, uint32_t now_ms);
/* Buttons are 0-based here; 1-based on the wire and for people. */
void lc_buttons_press(lc_buttons_t *bt, uint8_t i, uint32_t now_ms);
void lc_buttons_release(lc_buttons_t *bt, uint8_t i, uint32_t now_ms);
/* Call every 100 ms or so. */
void lc_buttons_tick(lc_buttons_t *bt, uint32_t now_ms);
/* From the engine's hello and control callbacks. */
void lc_buttons_on_hello(lc_buttons_t *bt, const lc_peer_t *peer, const lc_hello_t *hello, uint32_t now_ms);
void lc_buttons_on_control(lc_buttons_t *bt, const lc_peer_t *peer, const lc_control_t *msg, uint32_t now_ms);
void lc_buttons_unpair(lc_buttons_t *bt, uint8_t i);

/* Partners of the held, linked buttons (each once): who to talk to now. */
size_t lc_buttons_talk_set(const lc_buttons_t *bt, uint32_t *devices, size_t cap);
/* Whether to play a stream from this peer: a partner, or an announcer. */
bool lc_buttons_accepts(const lc_buttons_t *bt, uint32_t sender_id, uint32_t caps);
/* `receiving`: sender ids of the streams playing now. */
lc_led_state_t lc_buttons_led(const lc_buttons_t *bt, uint8_t i, bool transmitting, const uint32_t *receiving,
                              size_t n_receiving, uint32_t now_ms);
/* Ring brightness 0..1 for a state; `idle` is the steady brightness of a linked ring. */
float lc_led_level(lc_led_state_t state, float idle, uint32_t now_ms);
/* Start-up sweep: ring i of n fades up and down, one after the other (1 -> n). `ms` is the
 * time since start; returns -1 once the sweep is over. */
#define LC_BOOT_FADE_MS 800
#define LC_BOOT_STEP_MS 250
float lc_led_boot_level(uint8_t i, uint8_t n, uint32_t ms);
/* Night: the hour (0-23) lies in [start, end), wrapping past midnight; start == end: never. */
bool lc_is_night(int hour, int start, int end);

#ifdef __cplusplus
}
#endif
