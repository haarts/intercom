/* Engines wired together through an in-memory LAN. */
#include "test.h"

#include "lanicom/buttons.h"
#include "lanicom/engine.h"

#define MAX_NODES 4
#define QUEUE 512

typedef struct {
  lc_engine_t e;
  lc_buttons_t bt;
  bool has_buttons;
  uint32_t ip;
  int added, removed, starts, stops, audio;
  uint32_t last_stream, last_ts;
  lc_target_t last_target;
  unsigned seed;
} node_t;

typedef struct {
  lc_addr_t from, to;
  uint16_t len;
  uint8_t data[LC_MAX_PACKET];
} pkt_t;

static node_t nodes[MAX_NODES];
static int n_nodes;
static pkt_t queue[QUEUE];
static int q_len;
static pkt_t captured[QUEUE];
static int n_captured;
static int capture_from = -1;
static uint32_t now;

static uint32_t rnd(void *ctx) {
  node_t *n = ctx;
  return (uint32_t)rand_r(&n->seed) << 16 ^ (uint32_t)rand_r(&n->seed);
}

static void net_send(void *ctx, lc_addr_t to, const uint8_t *data, size_t len) {
  node_t *n = ctx;
  if (q_len >= QUEUE)
    abort();
  pkt_t *p = &queue[q_len++];
  p->from = (lc_addr_t){n->ip, LC_PORT};
  p->to = to;
  p->len = (uint16_t)len;
  memcpy(p->data, data, len);
  if (capture_from >= 0 && n == &nodes[capture_from] && n_captured < QUEUE)
    captured[n_captured++] = *p;
}

static void on_added(void *ctx, const lc_peer_t *p) { (void)p, ((node_t *)ctx)->added++; }
static void on_removed(void *ctx, const lc_peer_t *p) { (void)p, ((node_t *)ctx)->removed++; }
static void on_start(void *ctx, const lc_peer_t *p, const lc_target_t *t, uint32_t sid) {
  node_t *n = ctx;
  (void)p, (void)sid;
  n->starts++;
  n->last_target = *t;
}
static void on_stop(void *ctx, const lc_peer_t *p, uint32_t sid) { (void)p, (void)sid, ((node_t *)ctx)->stops++; }
static void on_audio(void *ctx, const lc_peer_t *p, uint32_t sid, uint32_t ts, const uint8_t *o, size_t len) {
  node_t *n = ctx;
  (void)p, (void)o, (void)len;
  n->audio++;
  n->last_stream = sid;
  n->last_ts = ts;
}

static void on_hello(void *ctx, const lc_peer_t *p, const lc_hello_t *h) {
  node_t *n = ctx;
  if (n->has_buttons)
    lc_buttons_on_hello(&n->bt, p, h, now);
}
static void on_control(void *ctx, const lc_peer_t *p, const lc_control_t *msg) {
  node_t *n = ctx;
  if (n->has_buttons)
    lc_buttons_on_control(&n->bt, p, msg, now);
}

/* Drop the next `drop_n` unicast packets sent by node `drop_from` (to test retries). */
static int drop_from = -1;
static int drop_n;

static void deliver(void) {
  /* Deliver until quiet; broadcast goes to everyone else. */
  for (int rounds = 0; q_len && rounds < 50; rounds++) {
    static pkt_t batch[QUEUE];
    int n = q_len;
    memcpy(batch, queue, sizeof(pkt_t) * (size_t)n);
    q_len = 0;
    for (int i = 0; i < n; i++) {
      if (drop_from >= 0 && drop_n > 0 && batch[i].from.ip == nodes[drop_from].ip && batch[i].to.ip != LC_BROADCAST_IP) {
        drop_n--;
        continue;
      }
      for (int k = 0; k < n_nodes; k++) {
        node_t *dst = &nodes[k];
        bool to_me = batch[i].to.ip == dst->ip || batch[i].to.ip == LC_BROADCAST_IP ||
                     (batch[i].to.ip == LC_MULTICAST_IP && dst->e.multicast);
        if (to_me && batch[i].from.ip != dst->ip)
          lc_engine_receive(&dst->e, batch[i].data, batch[i].len, batch[i].from, now);
      }
    }
  }
}

static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    now += 10;
    for (int k = 0; k < n_nodes; k++) {
      lc_engine_tick(&nodes[k].e, now);
      if (nodes[k].has_buttons)
        lc_buttons_tick(&nodes[k].bt, now);
    }
    deliver();
  }
}

static node_t *add_node(const lc_key_t *key, const char *name) {
  node_t *n = &nodes[n_nodes];
  memset(n, 0, sizeof(*n));
  n->ip = 0x0A000001u + (uint32_t)n_nodes;
  n->seed = 1234u + (unsigned)n_nodes * 77u + now;
  lc_callbacks_t cb = {.send = net_send,
                       .random32 = rnd,
                       .peer_added = on_added,
                       .peer_removed = on_removed,
                       .talk_start = on_start,
                       .talk_stop = on_stop,
                       .audio = on_audio,
                       .hello = on_hello,
                       .control = on_control,
                       .ctx = n};
  lc_engine_init(&n->e, key, 0, 0, &cb, now);
  lc_engine_set_identity(&n->e, name, LC_CAP_PLAYBACK | LC_CAP_CAPTURE);
  n_nodes++;
  return n;
}

static void reset(void) {
  drop_from = -1;
  drop_n = 0;
  n_nodes = 0;
  q_len = 0;
  n_captured = 0;
  capture_from = -1;
  now = 1000;
}

static bool verified(node_t *a, node_t *b) {
  const lc_peer_t *p = lc_engine_find_peer(&a->e, b->e.sender_id);
  return p && p->verified && p->announced;
}

static lc_key_t key_a, key_b;

static void test_discovery_and_talk(void) {
  reset();
  node_t *a = add_node(&key_a, "Hall");
  node_t *b = add_node(&key_a, "Kids");
  node_t *c = add_node(&key_a, "Workshop");
  node_t *x = add_node(&key_b, "Neighbour");
  run(100);
  CHECK(verified(a, b) && verified(b, a) && verified(a, c) && verified(c, b));
  CHECK_EQ(lc_engine_peer_count(&a->e), 2);
  CHECK_EQ(a->added, 2);
  CHECK(lc_engine_find_peer(&a->e, x->e.sender_id) == NULL);
  CHECK_EQ(lc_engine_peer_count(&x->e), 0);
  CHECK(a->e.stats.foreign > 0);
  CHECK(strcmp(lc_engine_find_peer(&a->e, b->e.sender_id)->info.name, "Kids") == 0);

  lc_target_t t = {LC_TARGET_DEVICE, b->e.sender_id};
  lc_talk_t talk;
  lc_talk_begin(&a->e, &talk, &t);
  uint8_t opus[3] = {0x78, 1, 2};
  for (int i = 0; i < 5; i++) {
    CHECK_EQ(lc_talk_frame(&a->e, &talk, opus, sizeof(opus), 160), 1);
    run(10);
  }
  lc_talk_end(&a->e, &talk);
  run(10);
  CHECK_EQ(b->audio, 5);
  CHECK_EQ(c->audio, 0);
  CHECK_EQ(x->audio, 0);
  CHECK_EQ(b->starts, 1);
  CHECK_EQ(b->stops, 1);
  CHECK(b->last_target.kind == LC_TARGET_DEVICE && b->last_target.device == b->e.sender_id);
  CHECK_EQ(b->last_ts - talk.ts, (uint32_t)-480); /* last frame's ts */

  lc_target_parse("all", &t);
  lc_talk_begin(&a->e, &talk, &t);
  CHECK_EQ(lc_talk_frame(&a->e, &talk, opus, sizeof(opus), 160), 2);
  run(10);
  CHECK_EQ(c->audio, 1);

  /* Clean shutdown. */
  lc_engine_bye(&c->e);
  deliver();
  CHECK_EQ(a->removed, 1);
  CHECK(lc_engine_find_peer(&a->e, c->e.sender_id) == NULL);
}

static void test_expiry(void) {
  reset();
  node_t *a = add_node(&key_a, "A");
  node_t *b = add_node(&key_a, "B");
  run(100);
  CHECK(verified(a, b));
  n_nodes = 1; /* b vanishes without a word */
  run(LC_PEER_TIMEOUT_MS + 2000);
  CHECK_EQ(a->removed, 1);
  CHECK_EQ(lc_engine_peer_count(&a->e), 0);
}

static void test_replay_after_restart(void) {
  reset();
  node_t *a = add_node(&key_a, "A");
  node_t *b = add_node(&key_a, "B");
  run(100);
  capture_from = 0;
  lc_target_t t = {LC_TARGET_ALL, 0};
  lc_talk_t talk;
  lc_talk_begin(&a->e, &talk, &t);
  uint8_t opus[2] = {0x78, 1};
  for (int i = 0; i < 5; i++)
    lc_talk_frame(&a->e, &talk, opus, 2, 160);
  deliver();
  CHECK_EQ(b->audio, 5);
  int n = n_captured;
  capture_from = -1;

  /* Replay to b in the same epoch: all duplicates. */
  uint32_t replays = b->e.stats.replay;
  for (int i = 0; i < n; i++)
    lc_engine_receive(&b->e, captured[i].data, captured[i].len, captured[i].from, now);
  CHECK_EQ(b->audio, 5);
  CHECK_EQ(b->e.stats.replay - replays, (uint32_t)n);

  /* b restarts (fresh engine, same id): the old epoch is unverified; replays only trigger challenges. */
  uint32_t b_id = b->e.sender_id;
  lc_callbacks_t cb = b->e.cb;
  lc_engine_init(&b->e, &key_a, b_id, 0, &cb, now);
  b->audio = 0;
  q_len = 0;
  for (int i = 0; i < n; i++)
    lc_engine_receive(&b->e, captured[i].data, captured[i].len, captured[i].from, now);
  CHECK_EQ(b->audio, 0);
  CHECK(b->e.stats.unverified >= (uint32_t)n);
  CHECK_EQ(q_len, 1); /* one challenge, rate limited */
  q_len = 0;          /* the attacker can't answer it */
  const lc_peer_t *pa = lc_engine_find_peer(&b->e, a->e.sender_id);
  CHECK(pa && !pa->verified);

  /* The real a answers challenges, so b verifies it once they talk normally. */
  run(LC_HELLO_INTERVAL_MS + 1500);
  CHECK(verified(b, a));
}

static void test_sender_id_collision(void) {
  reset();
  node_t *a = add_node(&key_a, "A");
  node_t *b = add_node(&key_a, "B");
  b->e.sender_id = a->e.sender_id; /* both picked the same id */
  run(100);
  CHECK(a->e.sender_id != b->e.sender_id);
  CHECK(a->e.stats.collision + b->e.stats.collision > 0);
  run(2000);
  CHECK(verified(a, b) && verified(b, a));
}

static void test_multicast(void) {
  reset();
  node_t *a = add_node(&key_a, "A");
  node_t *b = add_node(&key_a, "B");
  node_t *c = add_node(&key_a, "C");
  a->e.multicast = b->e.multicast = c->e.multicast = true;
  run(100);
  q_len = 0;
  lc_target_t t = {LC_TARGET_ALL, 0};
  lc_talk_t talk;
  lc_talk_begin(&a->e, &talk, &t);
  uint32_t tx = a->e.stats.tx;
  uint8_t opus[2] = {0x78, 1};
  CHECK_EQ(lc_talk_frame(&a->e, &talk, opus, 2, 160), 2);
  CHECK_EQ(a->e.stats.tx - tx, 3); /* TalkStart unicast to both, the audio once to the group */
  deliver();
  CHECK_EQ(b->audio, 1);
  CHECK_EQ(c->audio, 1);
}

static void test_monitor(void) {
  reset();
  node_t *a = add_node(&key_a, "A");
  node_t *b = add_node(&key_a, "B");
  node_t *m = add_node(&key_a, "HA");
  lc_engine_set_identity(&m->e, "HA", LC_CAP_CAPTURE | LC_CAP_MONITOR);
  run(100);
  lc_target_t t = {LC_TARGET_DEVICE, b->e.sender_id};
  lc_talk_t talk;
  lc_talk_begin(&a->e, &talk, &t);
  uint8_t opus[2] = {0x78, 1};
  CHECK_EQ(lc_talk_frame(&a->e, &talk, opus, 2, 160), 1);
  lc_talk_end(&a->e, &talk);
  deliver();
  CHECK_EQ(b->audio, 1);
  CHECK_EQ(m->audio, 0);
  CHECK_EQ(m->starts, 1);
  CHECK_EQ(m->stops, 1);
  CHECK(m->last_target.kind == LC_TARGET_DEVICE);
}


static void test_control_codec_v11(void) {
  /* Bytes from the official protobuf library (python/tools/pbref.py). */
  static const struct {
    const char *name, *hex;
  } cases[] = {
      {"hello_links", "0a210a074b69746368656e18035a09080115c2013f8a18025a09080415040302011801"},
      {"offer", "5a0b09efcdab89674523011003"},
      {"accept", "620b09efcdab89674523011001"},
      {"confirm", "6a0909efcdab8967452301"},
  };
  lc_control_t msg[4];
  memset(msg, 0, sizeof(msg));
  msg[0].type = LC_MSG_HELLO;
  strcpy(msg[0].u.hello.name, "Kitchen");
  msg[0].u.hello.caps = 3;
  msg[0].u.hello.n_links = 2;
  msg[0].u.hello.links[0] = (lc_link_t){1, 0x8a3f01c2u, 2};
  msg[0].u.hello.links[1] = (lc_link_t){4, 0x01020304u, 1};
  msg[1].type = LC_MSG_PAIR_OFFER;
  msg[1].u.pair.nonce = 0x0123456789abcdefull;
  msg[1].u.pair.button = 3;
  msg[2].type = LC_MSG_PAIR_ACCEPT;
  msg[2].u.pair.nonce = 0x0123456789abcdefull;
  msg[2].u.pair.button = 1;
  msg[3].type = LC_MSG_PAIR_CONFIRM;
  msg[3].u.pair.nonce = 0x0123456789abcdefull;
  for (int i = 0; i < 4; i++) {
    uint8_t buf[128];
    char hex[260];
    int n = lc_control_encode(&msg[i], buf, sizeof(buf));
    CHECK(n > 0);
    hex_encode(buf, (size_t)(n > 0 ? n : 0), hex);
    if (strcmp(hex, cases[i].hex) != 0)
      fprintf(stderr, "%s: %s\n", cases[i].name, hex);
    CHECK(strcmp(hex, cases[i].hex) == 0);
    lc_control_t back;
    CHECK(lc_control_decode(buf, (size_t)n, &back) == 0);
    CHECK(memcmp(&back, &msg[i], sizeof(back)) == 0);
  }
  /* More links than we keep: the rest are skipped, not an error. */
  uint8_t buf[64] = {0x0a, 4 * (LC_MAX_LINKS + 2)};
  for (int i = 0; i < LC_MAX_LINKS + 2; i++) {
    uint8_t link[4] = {0x5a, 2, 0x08, (uint8_t)(i + 1)}; /* Link {button: i + 1} */
    memcpy(buf + 2 + 4 * i, link, 4);
  }
  lc_control_t back;
  CHECK(lc_control_decode(buf, 2 + 4 * (LC_MAX_LINKS + 2), &back) == 0);
  CHECK_EQ(back.u.hello.n_links, LC_MAX_LINKS);
  CHECK_EQ(back.u.hello.links[LC_MAX_LINKS - 1].button, LC_MAX_LINKS);
}

static void test_device_set_talk(void) {
  reset();
  node_t *a = add_node(&key_a, "A");
  node_t *b = add_node(&key_a, "B");
  node_t *c = add_node(&key_a, "C");
  node_t *d = add_node(&key_a, "D");
  run(100);
  uint32_t set[3] = {b->e.sender_id, c->e.sender_id, b->e.sender_id};
  lc_talk_t talk;
  lc_talk_begin_devices(&a->e, &talk, set, 3);
  CHECK_EQ(talk.n_devices, 2);
  uint8_t opus[2] = {0x78, 1};
  uint32_t tx = a->e.stats.tx;
  CHECK_EQ(lc_talk_frame(&a->e, &talk, opus, 2, 160), 2);
  CHECK_EQ(a->e.stats.tx - tx, 4); /* a TalkStart to each, the frame (sealed once) to each */
  deliver();
  CHECK_EQ(b->audio, 1);
  CHECK_EQ(c->audio, 1);
  CHECK_EQ(d->audio, 0);
  CHECK(b->last_target.device == b->e.sender_id && c->last_target.device == c->e.sender_id);

  /* d joins mid-stream, c leaves. */
  uint32_t set2[2] = {b->e.sender_id, d->e.sender_id};
  lc_talk_set_devices(&a->e, &talk, set2, 2);
  deliver();
  CHECK_EQ(c->stops, 1);
  for (int i = 0; i < 3; i++)
    lc_talk_frame(&a->e, &talk, opus, 2, 160);
  deliver();
  CHECK_EQ(b->audio, 4);
  CHECK_EQ(c->audio, 1);
  CHECK_EQ(d->audio, 3);
  CHECK_EQ(d->starts, 1);
  lc_talk_end(&a->e, &talk);
  deliver();
  CHECK_EQ(b->stops, 1);
  CHECK_EQ(d->stops, 1);
  CHECK_EQ(c->stops, 1);
}

/* --- buttons ------------------------------------------------------------------- */

static node_t *add_box(const char *name, uint8_t buttons) {
  node_t *n = add_node(&key_a, name);
  lc_buttons_init(&n->bt, &n->e, buttons);
  n->has_buttons = true;
  return n;
}

static void hold(node_t *n, uint8_t i, uint32_t ms) {
  lc_buttons_press(&n->bt, i, now);
  run(ms);
  lc_buttons_release(&n->bt, i, now);
}

static bool linked(node_t *a, uint8_t i, node_t *b, uint8_t j) {
  return a->bt.b[i].link.partner == b->e.sender_id && a->bt.b[i].link.partner_button == j + 1;
}

static lc_led_state_t led(node_t *n, uint8_t i) { return lc_buttons_led(&n->bt, i, false, NULL, 0, now); }

static void test_pairing(void) {
  reset();
  node_t *kitchen = add_box("Kitchen", 4);
  node_t *kid = add_box("Kid", 1);
  node_t *other = add_box("Workshop", 4);
  run(100);
  CHECK_EQ(led(kitchen, 0), LC_LED_OFF);

  /* A short press on an unlinked button: error flicker, nothing else. */
  hold(kid, 0, 200);
  CHECK_EQ(led(kid, 0), LC_LED_ERROR);
  run(LC_ERROR_MS);
  CHECK_EQ(led(kid, 0), LC_LED_OFF);

  /* Kitchen button 2 held 5 s: pairing mode, offers once a second. */
  hold(kitchen, 1, LC_PAIR_HOLD_MS + 100);
  CHECK(kitchen->bt.b[1].pairing);
  CHECK_EQ(led(kitchen, 1), LC_LED_PAIRING);
  CHECK(kid->bt.offers[0].sender == kitchen->e.sender_id);
  run(20000);
  CHECK(kitchen->bt.b[1].pairing);

  /* 20 s later the kid holds theirs: the two link up. */
  lc_buttons_press(&kid->bt, 0, now);
  run(LC_PAIR_HOLD_MS + 100);
  CHECK(linked(kitchen, 1, kid, 0));
  CHECK(linked(kid, 0, kitchen, 1));
  CHECK(!kitchen->bt.b[1].pairing && !kid->bt.b[0].pairing);
  CHECK(kitchen->bt.links_changed && kid->bt.links_changed);
  CHECK_EQ(led(kid, 0), LC_LED_LINKED);
  CHECK(other->bt.b[0].link.partner == 0);
  /* The kid is still holding the button from pairing: that is not a talk. */
  uint32_t devs[4];
  CHECK_EQ(lc_buttons_talk_set(&kid->bt, devs, 4), 0);
  lc_buttons_release(&kid->bt, 0, now);
  run(LC_FLASH_MS);
  CHECK_EQ(led(kid, 0), LC_LED_IDLE);

  /* Both list the link in their Hello. */
  const lc_peer_t *p = lc_engine_find_peer(&other->e, kid->e.sender_id);
  (void)p;
  CHECK_EQ(kid->e.self.n_links, 1);
  CHECK_EQ(kid->e.self.links[0].partner, kitchen->e.sender_id);

  /* Talking: a press on the linked button talks to the partner; a long hold stays a talk. */
  lc_buttons_press(&kid->bt, 0, now);
  CHECK_EQ(lc_buttons_talk_set(&kid->bt, devs, 4), 1);
  CHECK_EQ(devs[0], kitchen->e.sender_id);
  run(LC_PAIR_HOLD_MS + 1000);
  CHECK(!kid->bt.b[0].pairing);
  CHECK_EQ(lc_buttons_talk_set(&kid->bt, devs, 4), 1);
  lc_buttons_release(&kid->bt, 0, now);
  CHECK_EQ(lc_buttons_talk_set(&kid->bt, devs, 4), 0);

  /* Who may play on the kid's box: the kitchen and announcers, not the workshop. */
  CHECK(lc_buttons_accepts(&kid->bt, kitchen->e.sender_id, LC_CAP_PLAYBACK));
  CHECK(!lc_buttons_accepts(&kid->bt, other->e.sender_id, LC_CAP_PLAYBACK));
  CHECK(lc_buttons_accepts(&kid->bt, other->e.sender_id, LC_CAP_ANNOUNCER));

  /* The workshop pairs two buttons with the kitchen; holding all three talks to both at once. */
  hold(other, 0, LC_PAIR_HOLD_MS + 100);
  hold(kitchen, 0, LC_PAIR_HOLD_MS + 100);
  CHECK(linked(other, 0, kitchen, 0) && linked(kitchen, 0, other, 0));
  lc_buttons_press(&kitchen->bt, 0, now);
  lc_buttons_press(&kitchen->bt, 1, now);
  CHECK_EQ(lc_buttons_talk_set(&kitchen->bt, devs, 4), 2);
  lc_buttons_release(&kitchen->bt, 0, now);
  lc_buttons_release(&kitchen->bt, 1, now);

  /* Unpairing one side: the other drops its side when it hears the next Hello. */
  run(LC_LINK_GRACE_MS);
  lc_buttons_unpair(&kid->bt, 0);
  run(100);
  CHECK(kitchen->bt.b[1].link.partner == 0);
  CHECK(linked(kitchen, 0, other, 0)); /* the other link stays */
  run(LC_HELLO_INTERVAL_MS * 3);
  CHECK(linked(kitchen, 0, other, 0) && linked(other, 0, kitchen, 0));
}

static void test_pairing_edges(void) {
  reset();
  node_t *a = add_box("A", 2);
  node_t *b = add_box("B", 2);
  node_t *c = add_box("C", 2);
  run(100);

  /* Timeout: nobody else pairs within 60 s. */
  hold(a, 0, LC_PAIR_HOLD_MS + 100);
  run(LC_PAIR_WINDOW_MS);
  CHECK(!a->bt.b[0].pairing);
  CHECK_EQ(led(a, 0), LC_LED_ERROR);

  /* A short press cancels pairing mode. */
  hold(a, 0, LC_PAIR_HOLD_MS + 100);
  CHECK(a->bt.b[0].pairing);
  hold(a, 0, 100);
  CHECK(!a->bt.b[0].pairing);
  run(LC_PAIR_OFFER_FRESH_MS + 100); /* b and c forget a's offer */

  /* Both enter pairing mode within the same second: exactly one link, both sides. */
  lc_buttons_press(&a->bt, 1, now);
  lc_buttons_press(&b->bt, 1, now);
  run(LC_PAIR_HOLD_MS + 2000);
  CHECK(linked(a, 1, b, 1) && linked(b, 1, a, 1));
  lc_buttons_release(&a->bt, 1, now);
  lc_buttons_release(&b->bt, 1, now);

  /* The Confirm is lost: the accepter repeats its Accept and gets one. */
  hold(a, 0, LC_PAIR_HOLD_MS + 100);
  lc_buttons_press(&c->bt, 0, now);
  run(LC_PAIR_HOLD_MS - 10);
  drop_from = 0; /* a */
  drop_n = 1;
  run(20); /* c enters pairing mode and sends Accept; a links, its Confirm is dropped */
  CHECK(linked(a, 0, c, 0));
  CHECK(c->bt.b[0].pairing && c->bt.b[0].accept_peer == a->e.sender_id);
  run(LC_PAIR_ACCEPT_RETRY_MS + 100);
  CHECK(linked(c, 0, a, 0));
  lc_buttons_release(&c->bt, 0, now);

  /* A linked button is never paired again, even if held 5 s. */
  hold(b, 0, LC_PAIR_HOLD_MS + 100);
  hold(c, 1, LC_PAIR_HOLD_MS + 100);
  CHECK(linked(b, 0, c, 1));
  hold(a, 0, LC_PAIR_HOLD_MS + 100);
  CHECK(linked(a, 0, c, 0));
  CHECK(!a->bt.b[0].pairing);

  /* Partner offline: the ring dips; back online: steady. */
  run(LC_LINK_GRACE_MS);
  int keep = n_nodes;
  n_nodes = 2; /* c disappears */
  run(LC_PEER_TIMEOUT_MS + 2000);
  CHECK_EQ(led(a, 0), LC_LED_OFFLINE);
  CHECK(linked(a, 0, c, 0)); /* the link stays */
  n_nodes = keep;
  run(LC_HELLO_INTERVAL_MS + 2000);
  CHECK_EQ(led(a, 0), LC_LED_IDLE);
  CHECK(linked(a, 0, c, 0) && linked(c, 0, a, 0));

  /* LED levels. */
  CHECK(lc_led_level(LC_LED_OFF, 0.1f, 0) == 0.0f);
  CHECK(lc_led_level(LC_LED_IDLE, 0.1f, 1234) == 0.1f);
  CHECK(lc_led_level(LC_LED_TALKING, 0.1f, 0) == 1.0f);
  CHECK(lc_led_level(LC_LED_RECEIVING, 0.1f, 0) == 0.1f);
  CHECK(lc_led_level(LC_LED_RECEIVING, 0.1f, 500) == 1.0f);
  CHECK(lc_led_level(LC_LED_OFFLINE, 0.1f, 3050) == 0.0f);

  /* Boot sweep: ring 1 peaks first, ring 4 last; then it's over. */
  CHECK(lc_led_boot_level(0, 4, LC_BOOT_FADE_MS / 2) == 1.0f);
  CHECK(lc_led_boot_level(3, 4, LC_BOOT_FADE_MS / 2) == 0.0f);
  CHECK(lc_led_boot_level(3, 4, 3 * LC_BOOT_STEP_MS + LC_BOOT_FADE_MS / 2) == 1.0f);
  CHECK(lc_led_boot_level(0, 4, 3 * LC_BOOT_STEP_MS + LC_BOOT_FADE_MS) < 0.0f);
  CHECK(lc_led_boot_level(0, 1, LC_BOOT_FADE_MS / 2) == 1.0f);
  CHECK(lc_led_boot_level(0, 1, LC_BOOT_FADE_MS) < 0.0f);

  /* Night hours. */
  CHECK(lc_is_night(23, 22, 7) && lc_is_night(3, 22, 7) && !lc_is_night(7, 22, 7) && !lc_is_night(12, 22, 7));
  CHECK(lc_is_night(14, 13, 15) && !lc_is_night(15, 13, 15));
  CHECK(!lc_is_night(3, 5, 5) && !lc_is_night(-1, 22, 7));
}

void test_engine(void) {
  CHECK(lc_key_derive(&key_a, "engine-test-a", 1000) == 0);
  CHECK(lc_key_derive(&key_b, "engine-test-b", 1000) == 0);
  test_discovery_and_talk();
  test_expiry();
  test_replay_after_restart();
  test_sender_id_collision();
  test_multicast();
  test_monitor();
  test_control_codec_v11();
  test_device_set_talk();
  test_pairing();
  test_pairing_edges();
}
