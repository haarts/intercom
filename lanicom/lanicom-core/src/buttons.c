#include "lanicom/buttons.h"

#include <string.h>

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

void lc_buttons_init(lc_buttons_t *bt, lc_engine_t *e, uint8_t n_buttons) {
  memset(bt, 0, sizeof(*bt));
  bt->engine = e;
  bt->n = n_buttons > LC_MAX_BUTTONS ? LC_MAX_BUTTONS : n_buttons;
}

/* --- links ------------------------------------------------------------------ */

static void publish_links(lc_buttons_t *bt) {
  lc_link_t links[LC_MAX_BUTTONS];
  size_t n = 0;
  for (uint8_t i = 0; i < bt->n; i++)
    if (bt->b[i].link.partner)
      links[n++] = (lc_link_t){i + 1u, bt->b[i].link.partner, bt->b[i].link.partner_button};
  lc_engine_set_links(bt->engine, links, n);
}

void lc_buttons_restore(lc_buttons_t *bt, const lc_button_link_t *links, size_t n, uint32_t now_ms) {
  for (uint8_t i = 0; i < bt->n && i < n; i++) {
    bt->b[i].link = links[i];
    bt->b[i].linked_ms = now_ms;
  }
  publish_links(bt);
}

static void stop_pairing(lc_button_t *b) {
  b->pairing = false;
  b->accept_peer = 0;
}

static void link_made(lc_buttons_t *bt, uint8_t i, uint32_t partner, uint8_t partner_button, uint64_t nonce,
                      uint32_t now_ms) {
  lc_button_t *b = &bt->b[i];
  stop_pairing(b);
  b->link = (lc_button_link_t){partner, partner_button};
  b->link_nonce = nonce;
  b->linked_ms = now_ms;
  b->flash_until_ms = now_ms + LC_FLASH_MS;
  b->error_until_ms = 0;
  bt->links_changed = true;
  publish_links(bt); /* still held after pairing: that hold doesn't become a talk (hold_done) */
}

static void drop_link(lc_buttons_t *bt, uint8_t i) {
  lc_button_t *b = &bt->b[i];
  if (!b->link.partner)
    return;
  if (b->down)
    bt->talk_changed = true;
  b->link = (lc_button_link_t){0, 0};
  b->link_nonce = 0;
  bt->links_changed = true;
  publish_links(bt);
}

void lc_buttons_unpair(lc_buttons_t *bt, uint8_t i) {
  if (i < bt->n)
    drop_link(bt, i);
}

/* Links heal themselves: a partner whose Hello doesn't list our link back has dropped it. */
void lc_buttons_on_hello(lc_buttons_t *bt, const lc_peer_t *peer, const lc_hello_t *hello, uint32_t now_ms) {
  uint32_t self = bt->engine->sender_id;
  for (uint8_t i = 0; i < bt->n; i++) {
    const lc_button_t *b = &bt->b[i];
    if (b->link.partner != peer->sender_id || (int32_t)(now_ms - b->linked_ms) < LC_LINK_GRACE_MS)
      continue;
    bool listed = false;
    for (uint8_t k = 0; k < hello->n_links && !listed; k++)
      listed = hello->links[k].button == b->link.partner_button && hello->links[k].partner == self &&
               hello->links[k].partner_button == i + 1u;
    if (!listed)
      drop_link(bt, i);
  }
}

/* --- pairing ---------------------------------------------------------------- */

static void send_pair(lc_buttons_t *bt, lc_msg_type_t type, uint32_t to, uint64_t nonce, uint32_t button) {
  lc_control_t msg = {.type = type};
  msg.u.pair.nonce = nonce;
  msg.u.pair.button = button;
  if (to)
    lc_engine_send_control(bt->engine, to, &msg);
  else
    lc_engine_broadcast_control(bt->engine, &msg);
}

static bool offer_taken(const lc_buttons_t *bt, const lc_pair_offer_t *o) {
  for (uint8_t i = 0; i < bt->n; i++)
    if (bt->b[i].accept_peer == o->sender && bt->b[i].accept_nonce == o->nonce)
      return true;
  return false;
}

static void accept(lc_buttons_t *bt, uint8_t i, const lc_pair_offer_t *o, uint32_t now_ms) {
  lc_button_t *b = &bt->b[i];
  b->accept_peer = o->sender;
  b->accept_nonce = o->nonce;
  b->accept_peer_button = o->button;
  b->accept_until_ms = now_ms + LC_PAIR_ACCEPT_GIVEUP_MS;
  b->accept_next_ms = now_ms + LC_PAIR_ACCEPT_RETRY_MS;
  send_pair(bt, LC_MSG_PAIR_ACCEPT, o->sender, o->nonce, i + 1u);
}

static void start_pairing(lc_buttons_t *bt, uint8_t i, uint32_t now_ms) {
  lc_button_t *b = &bt->b[i];
  b->pairing = true;
  b->nonce = lc_engine_random64(bt->engine);
  b->pairing_until_ms = now_ms + LC_PAIR_WINDOW_MS;
  b->accept_peer = 0;
  b->error_until_ms = 0;
  /* Someone already waiting? Take the oldest offer. */
  const lc_pair_offer_t *best = NULL;
  for (size_t k = 0; k < sizeof(bt->offers) / sizeof(bt->offers[0]); k++) {
    const lc_pair_offer_t *o = &bt->offers[k];
    if (o->sender && !offer_taken(bt, o) && (!best || (int32_t)(o->first_ms - best->first_ms) < 0))
      best = o;
  }
  if (best)
    accept(bt, i, best, now_ms);
  else
    b->next_offer_ms = now_ms;
}

static void forget_offer(lc_buttons_t *bt, uint32_t sender, uint64_t nonce) {
  for (size_t k = 0; k < sizeof(bt->offers) / sizeof(bt->offers[0]); k++)
    if (bt->offers[k].sender == sender && bt->offers[k].nonce == nonce)
      bt->offers[k].sender = 0;
}

static void on_offer(lc_buttons_t *bt, uint32_t sender, uint64_t nonce, uint8_t button, uint32_t now_ms) {
  lc_pair_offer_t *slot = NULL, *spare = NULL;
  for (size_t k = 0; k < sizeof(bt->offers) / sizeof(bt->offers[0]); k++) {
    lc_pair_offer_t *o = &bt->offers[k];
    if (o->sender == sender && o->nonce == nonce)
      slot = o;
    /* A free slot, else the one heard from longest ago. */
    if (!spare || (spare->sender && (!o->sender || (int32_t)(o->last_ms - spare->last_ms) < 0)))
      spare = o;
  }
  if (!slot) {
    slot = spare;
    *slot = (lc_pair_offer_t){sender, nonce, button, now_ms, now_ms};
  }
  slot->last_ms = now_ms;
  if (offer_taken(bt, slot))
    return;
  /* Two buttons offering at once: exactly one side accepts, the one with the larger nonce. */
  for (uint8_t i = 0; i < bt->n; i++) {
    lc_button_t *b = &bt->b[i];
    if (b->pairing && !b->accept_peer && nonce < b->nonce) {
      accept(bt, i, slot, now_ms);
      return;
    }
  }
}

void lc_buttons_on_control(lc_buttons_t *bt, const lc_peer_t *peer, const lc_control_t *msg, uint32_t now_ms) {
  uint32_t from = peer->sender_id;
  uint64_t nonce = msg->u.pair.nonce;
  uint32_t button = msg->u.pair.button;
  if (!nonce)
    return;
  switch (msg->type) {
    case LC_MSG_PAIR_OFFER:
      if (button >= 1 && button <= 255)
        on_offer(bt, from, nonce, (uint8_t)button, now_ms);
      break;
    case LC_MSG_PAIR_ACCEPT:
      if (button < 1 || button > 255)
        break;
      for (uint8_t i = 0; i < bt->n; i++) {
        lc_button_t *b = &bt->b[i];
        if (b->pairing && !b->accept_peer && b->nonce == nonce) {
          send_pair(bt, LC_MSG_PAIR_CONFIRM, from, nonce, 0); /* before link_made's Hello */
          link_made(bt, i, from, (uint8_t)button, nonce, now_ms);
          return;
        }
        if (b->link.partner == from && b->link.partner_button == button && b->link_nonce == nonce) {
          send_pair(bt, LC_MSG_PAIR_CONFIRM, from, nonce, 0); /* our first Confirm got lost */
          return;
        }
      }
      break;
    case LC_MSG_PAIR_CONFIRM:
      for (uint8_t i = 0; i < bt->n; i++) {
        lc_button_t *b = &bt->b[i];
        if (b->pairing && b->accept_peer == from && b->accept_nonce == nonce) {
          forget_offer(bt, from, nonce);
          link_made(bt, i, from, b->accept_peer_button, nonce, now_ms);
          return;
        }
      }
      break;
    default:
      break;
  }
}

/* --- the physical buttons ---------------------------------------------------- */

void lc_buttons_press(lc_buttons_t *bt, uint8_t i, uint32_t now_ms) {
  if (i >= bt->n || bt->b[i].down)
    return;
  lc_button_t *b = &bt->b[i];
  b->down = true;
  b->down_ms = now_ms;
  b->hold_done = false;
  if (b->link.partner)
    bt->talk_changed = true;
}

void lc_buttons_release(lc_buttons_t *bt, uint8_t i, uint32_t now_ms) {
  if (i >= bt->n || !bt->b[i].down)
    return;
  lc_button_t *b = &bt->b[i];
  b->down = false;
  if (b->link.partner) {
    bt->talk_changed = true;
  } else if (!b->hold_done) {
    if (b->pairing)
      stop_pairing(b); /* a short press cancels pairing */
    else
      b->error_until_ms = now_ms + LC_ERROR_MS;
  }
}

void lc_buttons_tick(lc_buttons_t *bt, uint32_t now_ms) {
  for (size_t k = 0; k < sizeof(bt->offers) / sizeof(bt->offers[0]); k++)
    if (bt->offers[k].sender && (int32_t)(now_ms - bt->offers[k].last_ms) > LC_PAIR_OFFER_FRESH_MS)
      bt->offers[k].sender = 0;

  for (uint8_t i = 0; i < bt->n; i++) {
    lc_button_t *b = &bt->b[i];
    /* A linked button never enters pairing mode: a long hold is a long talk. */
    if (b->down && !b->hold_done && !b->link.partner && reached(now_ms, b->down_ms + LC_PAIR_HOLD_MS)) {
      b->hold_done = true;
      if (!b->pairing)
        start_pairing(bt, i, now_ms);
    }
    if (!b->pairing)
      continue;
    if (reached(now_ms, b->pairing_until_ms)) {
      stop_pairing(b);
      b->error_until_ms = now_ms + LC_ERROR_MS;
    } else if (b->accept_peer) {
      if (reached(now_ms, b->accept_until_ms)) { /* the offerer paired with someone else, or left */
        forget_offer(bt, b->accept_peer, b->accept_nonce);
        b->accept_peer = 0;
        b->next_offer_ms = now_ms;
      } else if (reached(now_ms, b->accept_next_ms)) {
        b->accept_next_ms = now_ms + LC_PAIR_ACCEPT_RETRY_MS;
        send_pair(bt, LC_MSG_PAIR_ACCEPT, b->accept_peer, b->accept_nonce, i + 1u);
      }
    }
    if (b->pairing && !b->accept_peer && reached(now_ms, b->next_offer_ms)) {
      b->next_offer_ms = now_ms + LC_PAIR_OFFER_MS;
      send_pair(bt, LC_MSG_PAIR_OFFER, 0, b->nonce, i + 1u);
    }
  }
}

/* --- reading back ------------------------------------------------------------ */

size_t lc_buttons_talk_set(const lc_buttons_t *bt, uint32_t *devices, size_t cap) {
  size_t n = 0;
  for (uint8_t i = 0; i < bt->n; i++) {
    uint32_t p = bt->b[i].link.partner;
    if (!bt->b[i].down || bt->b[i].hold_done || !p)
      continue;
    bool dup = false;
    for (size_t k = 0; k < n && !dup; k++)
      dup = devices[k] == p;
    if (!dup && n < cap)
      devices[n++] = p;
  }
  return n;
}

bool lc_buttons_accepts(const lc_buttons_t *bt, uint32_t sender_id, uint32_t caps) {
  if (caps & LC_CAP_ANNOUNCER)
    return true;
  for (uint8_t i = 0; i < bt->n; i++)
    if (bt->b[i].link.partner == sender_id)
      return true;
  return false;
}

lc_led_state_t lc_buttons_led(const lc_buttons_t *bt, uint8_t i, bool transmitting, const uint32_t *receiving,
                              size_t n_receiving, uint32_t now_ms) {
  if (i >= bt->n)
    return LC_LED_OFF;
  const lc_button_t *b = &bt->b[i];
  if (b->error_until_ms && !reached(now_ms, b->error_until_ms))
    return LC_LED_ERROR;
  if (b->pairing)
    return LC_LED_PAIRING;
  if (!b->link.partner)
    return LC_LED_OFF;
  if (b->flash_until_ms && !reached(now_ms, b->flash_until_ms))
    return LC_LED_LINKED;
  if (b->down && !b->hold_done && transmitting)
    return LC_LED_TALKING;
  for (size_t k = 0; k < n_receiving; k++)
    if (receiving[k] == b->link.partner)
      return LC_LED_RECEIVING;
  const lc_peer_t *p = lc_engine_find_peer(bt->engine, b->link.partner);
  return p && p->verified && p->announced ? LC_LED_IDLE : LC_LED_OFFLINE;
}

float lc_led_level(lc_led_state_t state, float idle, uint32_t now_ms) {
  switch (state) {
    case LC_LED_IDLE:
      return idle;
    case LC_LED_OFFLINE:
      return now_ms % 3000 < 150 ? 0.0f : idle; /* a short dip every 3 s */
    case LC_LED_TALKING:
      return 1.0f;
    case LC_LED_RECEIVING: { /* breathing, 1 Hz, idle <-> full */
      float p = (float)(now_ms % 1000) / 1000.0f;
      float t = p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p;
      return idle + (1.0f - idle) * t * t * (3.0f - 2.0f * t);
    }
    case LC_LED_PAIRING:
      return now_ms % 1000 < 500 ? 1.0f : 0.0f;
    case LC_LED_LINKED:
      return now_ms % 300 < 150 ? 1.0f : 0.0f;
    case LC_LED_ERROR:
      return now_ms % 125 < 62 ? 1.0f : 0.0f;
    case LC_LED_OFF:
    default:
      return 0.0f;
  }
}
