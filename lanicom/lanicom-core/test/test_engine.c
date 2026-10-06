/* Engines wired together through an in-memory LAN. */
#include "test.h"

#include "lanicom/engine.h"

#define MAX_NODES 4
#define QUEUE 512

typedef struct {
  lc_engine_t e;
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

static void deliver(void) {
  /* Deliver until quiet; broadcast goes to everyone else. */
  for (int rounds = 0; q_len && rounds < 50; rounds++) {
    static pkt_t batch[QUEUE];
    int n = q_len;
    memcpy(batch, queue, sizeof(pkt_t) * (size_t)n);
    q_len = 0;
    for (int i = 0; i < n; i++)
      for (int k = 0; k < n_nodes; k++) {
        node_t *dst = &nodes[k];
        bool to_me = batch[i].to.ip == dst->ip || batch[i].to.ip == LC_BROADCAST_IP ||
                     (batch[i].to.ip == LC_MULTICAST_IP && dst->e.multicast);
        if (to_me && batch[i].from.ip != dst->ip)
          lc_engine_receive(&dst->e, batch[i].data, batch[i].len, batch[i].from, now);
      }
  }
}

static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    now += 10;
    for (int k = 0; k < n_nodes; k++)
      lc_engine_tick(&nodes[k].e, now);
    deliver();
  }
}

static node_t *add_node(const lc_key_t *key, const char *name) {
  node_t *n = &nodes[n_nodes];
  memset(n, 0, sizeof(*n));
  n->ip = 0x0A000001u + (uint32_t)n_nodes;
  n->seed = 1234u + (unsigned)n_nodes * 77u + now;
  lc_callbacks_t cb = {net_send, rnd, on_added, on_removed, NULL, on_start, on_stop, on_audio, NULL, n};
  lc_engine_init(&n->e, key, 0, 0, &cb, now);
  lc_engine_set_identity(&n->e, name, LC_CAP_PLAYBACK | LC_CAP_CAPTURE);
  n_nodes++;
  return n;
}

static void reset(void) {
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

void test_engine(void) {
  CHECK(lc_key_derive(&key_a, "engine-test-a", 1000) == 0);
  CHECK(lc_key_derive(&key_b, "engine-test-b", 1000) == 0);
  test_discovery_and_talk();
  test_expiry();
  test_replay_after_restart();
  test_sender_id_collision();
  test_multicast();
  test_monitor();
}
