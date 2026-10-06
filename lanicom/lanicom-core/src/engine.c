#include "lanicom/engine.h"

#include <stdio.h>
#include <string.h>

#define AUDIO_HDR 8
#define LC_TICKS_PER_SAMPLE_ENGINE 3 /* 48 kHz ts ticks per 16 kHz sample */

static uint32_t rnd32(lc_engine_t *e) { return e->cb.random32(e->cb.ctx); }
static uint64_t rnd64_nonzero(lc_engine_t *e) {
  uint64_t v;
  do
    v = (uint64_t)rnd32(e) << 32 | rnd32(e);
  while (v == 0);
  return v;
}
static uint32_t rnd32_nonzero(lc_engine_t *e) {
  uint32_t v;
  do
    v = rnd32(e);
  while (v == 0);
  return v;
}
static bool time_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

void lc_engine_init(lc_engine_t *e, const lc_key_t *key, uint32_t sender_id, uint64_t epoch,
                    const lc_callbacks_t *cb, uint32_t now_ms) {
  memset(e, 0, sizeof(*e));
  e->key = key;
  e->cb = *cb;
  e->sender_id = sender_id ? sender_id : rnd32_nonzero(e);
  e->epoch = epoch ? epoch : rnd64_nonzero(e);
  e->port = LC_PORT;
  e->broadcast = true;
  e->self.caps = LC_CAP_PLAYBACK | LC_CAP_CAPTURE;
  e->next_hello_ms = now_ms;
  e->next_expiry_ms = now_ms + 1000;
}

/* --- sending ------------------------------------------------------------- */

static void send_raw(lc_engine_t *e, lc_addr_t to, const uint8_t *data, size_t len) {
  e->cb.send(e->cb.ctx, to, data, len);
  e->stats.tx++;
}

/* Seal into e->tx; returns the length or < 0. */
static int seal(lc_engine_t *e, uint8_t type, const uint8_t *plain, size_t len) {
  if (e->seq_wrapped) {
    e->epoch = rnd64_nonzero(e);
    e->seq = 0;
    e->seq_wrapped = false;
  }
  lc_header_t h = {LC_VERSION, type, e->key->key_id, e->sender_id, e->epoch, e->seq};
  uint8_t key[32];
  if (lc_send_key(e->key, e->sender_id, key))
    return -1;
  if (++e->seq == 0)
    e->seq_wrapped = true;
  return lc_seal(key, &h, plain, len, e->tx, sizeof(e->tx));
}

static int seal_control(lc_engine_t *e, const lc_control_t *msg) {
  uint8_t plain[LC_MAX_PLAINTEXT];
  int n = lc_control_encode(msg, plain, sizeof(plain));
  return n < 0 ? -1 : seal(e, LC_TYPE_CONTROL, plain, (size_t)n);
}

static void hello_msg(const lc_engine_t *e, lc_control_t *msg, uint64_t challenge, uint64_t echo, bool bye) {
  memset(msg, 0, sizeof(*msg));
  msg->type = LC_MSG_HELLO;
  msg->u.hello = e->self;
  msg->u.hello.challenge = challenge;
  msg->u.hello.echo = echo;
  msg->u.hello.bye = bye;
}

static void send_hello_to(lc_engine_t *e, lc_addr_t to, uint64_t challenge, uint64_t echo, bool bye) {
  lc_control_t msg;
  hello_msg(e, &msg, challenge, echo, bye);
  int n = seal_control(e, &msg);
  if (n > 0)
    send_raw(e, to, e->tx, (size_t)n);
}

static void broadcast_hello(lc_engine_t *e, bool bye) {
  lc_control_t msg;
  hello_msg(e, &msg, 0, 0, bye);
  int n = seal_control(e, &msg);
  if (n <= 0)
    return;
  if (e->broadcast)
    send_raw(e, (lc_addr_t){LC_BROADCAST_IP, e->port}, e->tx, (size_t)n);
  for (uint8_t i = 0; i < e->n_static; i++)
    send_raw(e, e->statics[i], e->tx, (size_t)n);
}

static void challenge(lc_engine_t *e, lc_peer_t *p, uint32_t now_ms) {
  if (p->challenge_ever_sent && (int32_t)(now_ms - p->challenge_sent_ms) < LC_CHALLENGE_INTERVAL_MS)
    return;
  p->challenge = rnd64_nonzero(e);
  p->challenge_sent_ms = now_ms;
  p->challenge_ever_sent = true;
  send_hello_to(e, p->addr, p->challenge, 0, false);
}

static bool recipient(const lc_peer_t *p, const lc_target_t *t) {
  return p->in_use && p->verified && (p->info.caps & LC_CAP_PLAYBACK) && lc_target_matches(t, p->sender_id, &p->info);
}

size_t lc_engine_recipient_count(const lc_engine_t *e, const lc_target_t *target) {
  size_t n = 0;
  for (int i = 0; i < LC_MAX_PEERS; i++)
    n += recipient(&e->peers[i], target);
  return n;
}

/* Seal once, then multicast or fan out. Returns the number of recipients. */
static int send_to_target(lc_engine_t *e, uint8_t type, const uint8_t *plain, size_t len, const lc_target_t *t) {
  size_t n = lc_engine_recipient_count(e, t);
  if (n == 0)
    return 0;
  int plen = seal(e, type, plain, len);
  if (plen <= 0)
    return 0;
  if (e->multicast && t->kind == LC_TARGET_ALL) {
    send_raw(e, (lc_addr_t){LC_MULTICAST_IP, e->port}, e->tx, (size_t)plen);
  } else {
    for (int i = 0; i < LC_MAX_PEERS; i++)
      if (recipient(&e->peers[i], t))
        send_raw(e, e->peers[i].addr, e->tx, (size_t)plen);
  }
  return (int)n;
}

/* TalkStart/TalkStop: unicast to the stream's recipients and every monitor. */
static void send_talk_metadata(lc_engine_t *e, const lc_control_t *msg, const lc_target_t *t) {
  bool any = false;
  for (int i = 0; i < LC_MAX_PEERS && !any; i++)
    any = recipient(&e->peers[i], t) || (e->peers[i].in_use && e->peers[i].verified && (e->peers[i].info.caps & LC_CAP_MONITOR));
  if (!any)
    return;
  int n = seal_control(e, msg);
  if (n <= 0)
    return;
  for (int i = 0; i < LC_MAX_PEERS; i++) {
    const lc_peer_t *p = &e->peers[i];
    if (recipient(p, t) || (p->in_use && p->verified && (p->info.caps & LC_CAP_MONITOR)))
      send_raw(e, p->addr, e->tx, (size_t)n);
  }
}

/* --- identity ------------------------------------------------------------ */

void lc_engine_set_identity(lc_engine_t *e, const char *name, const char *zones, uint32_t caps) {
  snprintf(e->self.name, sizeof(e->self.name), "%s", name ? name : "");
  e->self.n_zones = 0;
  e->self.caps = caps;
  const char *p = zones ? zones : "";
  while (*p && e->self.n_zones < LC_ZONES_MAX) {
    while (*p == ',' || *p == ' ')
      p++;
    const char *end = p;
    while (*end && *end != ',')
      end++;
    size_t n = (size_t)(end - p);
    while (n && p[n - 1] == ' ')
      n--;
    if (n) {
      if (n > LC_ZONE_MAX)
        n = LC_ZONE_MAX;
      char *z = e->self.zones[e->self.n_zones++];
      for (size_t i = 0; i < n; i++)
        z[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
      z[n] = 0;
    }
    p = end;
  }
  if (e->startup_hellos >= 3) { /* already running: tell everyone now */
    broadcast_hello(e, false);
    for (int i = 0; i < LC_MAX_PEERS; i++)
      if (e->peers[i].in_use && e->peers[i].verified)
        send_hello_to(e, e->peers[i].addr, 0, 0, false);
  }
}

int lc_engine_add_static_peer(lc_engine_t *e, lc_addr_t addr) {
  if (e->n_static >= LC_MAX_STATIC)
    return -1;
  e->statics[e->n_static++] = addr;
  return 0;
}

/* --- peers --------------------------------------------------------------- */

const lc_peer_t *lc_engine_find_peer(const lc_engine_t *e, uint32_t sender_id) {
  for (int i = 0; i < LC_MAX_PEERS; i++)
    if (e->peers[i].in_use && e->peers[i].sender_id == sender_id)
      return &e->peers[i];
  return NULL;
}

size_t lc_engine_peer_count(const lc_engine_t *e) {
  size_t n = 0;
  for (int i = 0; i < LC_MAX_PEERS; i++)
    n += e->peers[i].in_use && e->peers[i].verified && e->peers[i].announced;
  return n;
}

static lc_peer_t *get_or_create(lc_engine_t *e, uint32_t sender_id, lc_addr_t addr, uint32_t now_ms) {
  lc_peer_t *free_slot = NULL;
  for (int i = 0; i < LC_MAX_PEERS; i++) {
    lc_peer_t *p = &e->peers[i];
    if (p->in_use && p->sender_id == sender_id)
      return p;
    if (!p->in_use && !free_slot)
      free_slot = p;
  }
  if (!free_slot)
    return NULL;
  memset(free_slot, 0, sizeof(*free_slot));
  if (lc_send_key(e->key, sender_id, free_slot->send_key))
    return NULL;
  free_slot->in_use = true;
  free_slot->sender_id = sender_id;
  free_slot->addr = addr;
  free_slot->first_seen_ms = free_slot->last_seen_ms = now_ms;
  free_slot->rtt_ms = -1;
  return free_slot;
}

static void remove_peer(lc_engine_t *e, lc_peer_t *p) {
  if (p->announced && e->cb.peer_removed)
    e->cb.peer_removed(e->cb.ctx, p);
  p->in_use = false;
}

/* --- receiving ----------------------------------------------------------- */

static bool echo_allowed(lc_engine_t *e, uint32_t ip, uint32_t now_ms) {
  lc_echo_budget_t *slot = NULL, *oldest = &e->echo[0];
  for (size_t i = 0; i < sizeof(e->echo) / sizeof(e->echo[0]); i++) {
    if (e->echo[i].ip == ip && e->echo[i].count)
      slot = &e->echo[i];
    if (time_after(oldest->window_ms, e->echo[i].window_ms) || !e->echo[i].count)
      oldest = &e->echo[i];
  }
  if (!slot || (int32_t)(now_ms - slot->window_ms) >= 1000) {
    if (!slot)
      slot = oldest;
    slot->ip = ip;
    slot->window_ms = now_ms;
    slot->count = 0;
  }
  if (slot->count >= LC_ECHO_LIMIT_PER_S)
    return false;
  slot->count++;
  return true;
}

static lc_talk_seen_t *talk_seen(lc_engine_t *e, uint32_t sender_id, uint32_t stream_id) {
  for (size_t i = 0; i < sizeof(e->talks) / sizeof(e->talks[0]); i++)
    if (e->talks[i].sender_id == sender_id && e->talks[i].stream_id == stream_id && e->talks[i].stream_id)
      return &e->talks[i];
  return NULL;
}

static lc_talk_seen_t *talk_remember(lc_engine_t *e, uint32_t sender_id, uint32_t stream_id, uint32_t now_ms) {
  lc_talk_seen_t *t = &e->talks[e->talk_next];
  e->talk_next = (uint8_t)((e->talk_next + 1) % (sizeof(e->talks) / sizeof(e->talks[0])));
  t->sender_id = sender_id;
  t->stream_id = stream_id;
  t->at_ms = now_ms;
  t->stopped = false;
  return t;
}

static void handle_control(lc_engine_t *e, lc_peer_t *p, const lc_control_t *msg, uint32_t now_ms) {
  e->stats.rx_control++;
  if (msg->type == LC_MSG_HELLO) {
    const lc_hello_t *h = &msg->u.hello;
    if (h->bye) {
      remove_peer(e, p);
      return;
    }
    bool changed = strcmp(h->name, p->info.name) != 0 || h->caps != p->info.caps || h->n_zones != p->info.n_zones;
    for (uint8_t i = 0; !changed && i < h->n_zones; i++)
      changed = strcmp(h->zones[i], p->info.zones[i]) != 0;
    memcpy(p->info.name, h->name, sizeof(p->info.name));
    memcpy(p->info.zones, h->zones, sizeof(p->info.zones));
    p->info.n_zones = h->n_zones;
    p->info.caps = h->caps;
    if (!p->announced) {
      p->announced = true;
      if (e->cb.peer_added)
        e->cb.peer_added(e->cb.ctx, p);
    } else if (changed && e->cb.peer_updated) {
      e->cb.peer_updated(e->cb.ctx, p);
    }
  } else if (msg->type == LC_MSG_TALK_START) {
    if (!talk_seen(e, p->sender_id, msg->u.talk_start.stream_id)) {
      talk_remember(e, p->sender_id, msg->u.talk_start.stream_id, now_ms);
      if (e->cb.talk_start)
        e->cb.talk_start(e->cb.ctx, p, &msg->u.talk_start.target, msg->u.talk_start.stream_id);
    }
  } else if (msg->type == LC_MSG_TALK_STOP) {
    lc_talk_seen_t *t = talk_seen(e, p->sender_id, msg->u.talk_stop.stream_id);
    if (!t)
      t = talk_remember(e, p->sender_id, msg->u.talk_stop.stream_id, now_ms);
    else if (t->stopped)
      return;
    t->stopped = true;
    if (e->cb.talk_stop)
      e->cb.talk_stop(e->cb.ctx, p, msg->u.talk_stop.stream_id);
  }
}

void lc_engine_receive(lc_engine_t *e, const uint8_t *data, size_t len, lc_addr_t from, uint32_t now_ms) {
  lc_header_t h;
  if (lc_header_parse(data, len, &h) != LC_OK) {
    e->stats.malformed++;
    return;
  }
  if (h.key_id != e->key->key_id) {
    e->stats.foreign++;
    return;
  }
  bool own = h.sender_id == e->sender_id;
  if (own && h.epoch == e->epoch)
    return; /* our own broadcast */

  lc_peer_t *p = NULL;
  uint8_t key[32];
  const uint8_t *k = key;
  for (int i = 0; i < LC_MAX_PEERS && !own; i++)
    if (e->peers[i].in_use && e->peers[i].sender_id == h.sender_id)
      p = &e->peers[i];
  if (p)
    k = p->send_key;
  else if (lc_send_key(e->key, h.sender_id, key))
    return;
  int plen = lc_open(k, data, len, e->rx_plain);
  if (plen < 0) {
    e->stats.auth++;
    return;
  }
  if (own) {
    e->stats.collision++;
    e->sender_id = rnd32_nonzero(e);
    if (e->cb.sender_id_changed)
      e->cb.sender_id_changed(e->cb.ctx, e->sender_id);
    return;
  }

  lc_control_t msg;
  bool is_control = h.type == LC_TYPE_CONTROL;
  if (is_control && lc_control_decode(e->rx_plain, (size_t)plen, &msg)) {
    e->stats.malformed++;
    return;
  }
  if (!p)
    p = get_or_create(e, h.sender_id, from, now_ms);
  if (!p) {
    e->stats.table_full++;
    return;
  }
  bool hello = is_control && msg.type == LC_MSG_HELLO;
  if (hello && msg.u.hello.challenge && echo_allowed(e, from.ip, now_ms))
    send_hello_to(e, from, 0, msg.u.hello.challenge, false);
  bool echo_ok = hello && p->challenge && msg.u.hello.echo == p->challenge;

  if (!p->verified || h.epoch != p->verified_epoch) {
    if (!echo_ok) {
      e->stats.unverified++;
      if (!p->verified)
        p->addr = from;
      challenge(e, p, now_ms);
      return;
    }
    p->verified = true;
    p->verified_epoch = h.epoch;
    lc_replay_init(&p->window, h.seq);
    p->rtt_ms = (int32_t)(now_ms - p->challenge_sent_ms);
    p->challenge = 0;
    e->stats.verified++;
  } else if (!lc_replay_check(&p->window, h.seq)) {
    e->stats.replay++;
    return;
  } else if (echo_ok) {
    p->rtt_ms = (int32_t)(now_ms - p->challenge_sent_ms);
    p->challenge = 0;
  }

  p->addr = from;
  p->last_seen_ms = now_ms;
  if (is_control) {
    handle_control(e, p, &msg, now_ms);
  } else {
    if (plen < AUDIO_HDR + 1) {
      e->stats.malformed++;
      return;
    }
    e->stats.rx_audio++;
    const uint8_t *a = e->rx_plain;
    uint32_t stream_id = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
    uint32_t ts = (uint32_t)a[4] << 24 | (uint32_t)a[5] << 16 | (uint32_t)a[6] << 8 | a[7];
    if (e->cb.audio)
      e->cb.audio(e->cb.ctx, p, stream_id, ts, a + AUDIO_HDR, (size_t)plen - AUDIO_HDR);
  }
}

/* --- periodic ------------------------------------------------------------ */

void lc_engine_tick(lc_engine_t *e, uint32_t now_ms) {
  static const uint32_t startup[] = {0, 500, 1500};
  if (!time_after(e->next_hello_ms, now_ms)) {
    broadcast_hello(e, false);
    if (e->startup_hellos < 3) {
      uint32_t prev = startup[e->startup_hellos];
      e->startup_hellos++;
      e->next_hello_ms = e->startup_hellos < 3 ? now_ms + startup[e->startup_hellos] - prev : now_ms + LC_HELLO_INTERVAL_MS;
    } else {
      e->next_hello_ms = now_ms + LC_HELLO_INTERVAL_MS - LC_HELLO_JITTER_MS + rnd32(e) % (2 * LC_HELLO_JITTER_MS);
      if (e->rtt_probe)
        for (int i = 0; i < LC_MAX_PEERS; i++)
          if (e->peers[i].in_use && e->peers[i].verified)
            challenge(e, &e->peers[i], now_ms);
    }
  }
  if (!time_after(e->next_expiry_ms, now_ms)) {
    e->next_expiry_ms = now_ms + 1000;
    for (int i = 0; i < LC_MAX_PEERS; i++)
      if (e->peers[i].in_use && (int32_t)(now_ms - e->peers[i].last_seen_ms) > LC_PEER_TIMEOUT_MS)
        remove_peer(e, &e->peers[i]);
  }
}

void lc_engine_bye(lc_engine_t *e) {
  /* Unicast only: a broadcast copy as well would arrive after the peer was removed and
   * re-create it as an unverified stranger. */
  for (int i = 0; i < LC_MAX_PEERS; i++)
    if (e->peers[i].in_use && e->peers[i].verified)
      send_hello_to(e, e->peers[i].addr, 0, 0, true);
}

/* --- talking ------------------------------------------------------------- */

void lc_talk_begin(lc_engine_t *e, lc_talk_t *talk, const lc_target_t *target) {
  memset(talk, 0, sizeof(*talk));
  talk->target = *target;
  talk->stream_id = rnd32_nonzero(e);
  talk->ts = rnd32(e);
  talk->active = true;
}

int lc_talk_frame(lc_engine_t *e, lc_talk_t *talk, const uint8_t *opus, size_t len, uint32_t samples) {
  if (!talk->active || len == 0 || len + AUDIO_HDR > LC_MAX_PLAINTEXT)
    return 0;
  if (talk->frames < 3) {
    lc_control_t msg = {.type = LC_MSG_TALK_START};
    msg.u.talk_start.target = talk->target;
    msg.u.talk_start.stream_id = talk->stream_id;
    send_talk_metadata(e, &msg, &talk->target);
  }
  uint8_t plain[LC_MAX_PLAINTEXT];
  uint32_t sid = talk->stream_id, ts = talk->ts;
  uint8_t hdr[AUDIO_HDR] = {(uint8_t)(sid >> 24), (uint8_t)(sid >> 16), (uint8_t)(sid >> 8), (uint8_t)sid,
                            (uint8_t)(ts >> 24),  (uint8_t)(ts >> 16),  (uint8_t)(ts >> 8),  (uint8_t)ts};
  memcpy(plain, hdr, AUDIO_HDR);
  memcpy(plain + AUDIO_HDR, opus, len);
  int n = send_to_target(e, LC_TYPE_AUDIO, plain, AUDIO_HDR + len, &talk->target);
  talk->ts += samples * LC_TICKS_PER_SAMPLE_ENGINE;
  talk->frames++;
  return n;
}

void lc_talk_end(lc_engine_t *e, lc_talk_t *talk) {
  if (!talk->active)
    return;
  talk->active = false;
  if (!talk->frames)
    return;
  lc_control_t msg = {.type = LC_MSG_TALK_STOP};
  msg.u.talk_stop.stream_id = talk->stream_id;
  for (int i = 0; i < 3; i++)
    send_talk_metadata(e, &msg, &talk->target);
}
