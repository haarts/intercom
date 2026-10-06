/* Shared test vectors from spec/vectors (the same ones the Python tests use). */
#include "test.h"
#include "vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static lc_key_t keys[8];
static const char *key_strings[8];
static size_t n_keys;

static const lc_key_t *key_for(const char *ks) {
  for (size_t i = 0; i < n_keys; i++)
    if (strcmp(key_strings[i], ks) == 0)
      return &keys[i];
  CHECK(lc_key_derive(&keys[n_keys], ks, LC_PBKDF2_ITERATIONS) == 0);
  key_strings[n_keys] = ks;
  return &keys[n_keys++];
}

static void test_keys(void) {
  for (size_t i = 0; i < N(KEY_VECS); i++) {
    const key_vec_t *v = &KEY_VECS[i];
    const lc_key_t *k = key_for(v->key_string);
    char hex[65];
    hex_encode(k->master, 32, hex);
    CHECK(strcmp(hex, v->master) == 0);
    CHECK_EQ(k->key_id, v->key_id);
    for (int s = 0; s < 3; s++) {
      uint8_t sk[32];
      CHECK(lc_send_key(k, v->sids[s], sk) == 0);
      hex_encode(sk, 32, hex);
      CHECK(strcmp(hex, v->send_keys[s]) == 0);
    }
  }
}

static void test_packets(void) {
  for (size_t i = 0; i < N(PACKET_VECS); i++) {
    const packet_vec_t *v = &PACKET_VECS[i];
    const lc_key_t *k = key_for(v->key_string);
    uint8_t sk[32], pt[LC_MAX_PACKET], pkt[LC_MAX_PACKET], expect[LC_MAX_PACKET];
    lc_send_key(k, v->sender_id, sk);
    size_t pt_len = hex_decode(v->plaintext, pt, sizeof(pt));
    size_t expect_len = hex_decode(v->packet, expect, sizeof(expect));
    lc_header_t h = {LC_VERSION, v->type, k->key_id, v->sender_id, v->epoch, v->seq};
    int n = lc_seal(sk, &h, pt, pt_len, pkt, sizeof(pkt));
    CHECK_EQ(n, expect_len);
    CHECK(memcmp(pkt, expect, expect_len) == 0);

    uint8_t out[LC_MAX_PACKET];
    lc_header_t parsed;
    CHECK(lc_header_parse(expect, expect_len, &parsed) == LC_OK);
    CHECK_EQ(parsed.sender_id, v->sender_id);
    CHECK(parsed.epoch == v->epoch);
    CHECK_EQ(parsed.seq, v->seq);
    CHECK_EQ(lc_open(sk, expect, expect_len, out), pt_len);
    CHECK(memcmp(out, pt, pt_len) == 0);
  }
}

static void test_invalid(void) {
  for (size_t i = 0; i < N(INVALID_VECS); i++) {
    const invalid_vec_t *v = &INVALID_VECS[i];
    const lc_key_t *k = key_for(v->key_string);
    uint8_t pkt[2600], out[2600];
    size_t len = hex_decode(v->packet, pkt, sizeof(pkt));
    lc_header_t h;
    const char *reason = "ok";
    if (lc_header_parse(pkt, len, &h) != LC_OK) {
      reason = "malformed";
    } else if (h.key_id != k->key_id) {
      reason = "foreign";
    } else {
      uint8_t sk[32];
      lc_send_key(k, h.sender_id, sk);
      if (lc_open(sk, pkt, len, out) < 0)
        reason = "auth";
    }
    if (strcmp(reason, v->reason) != 0)
      fprintf(stderr, "invalid packet %s: got %s, want %s\n", v->name, reason, v->reason);
    CHECK(strcmp(reason, v->reason) == 0);
  }
}

static bool target_eq(const lc_target_t *a, const lc_target_t *b) {
  return a->kind == b->kind && a->device == b->device && strcmp(a->zone, b->zone) == 0;
}

static bool control_eq(const lc_control_t *a, const lc_control_t *b) {
  if (a->type != b->type)
    return false;
  if (a->type == LC_MSG_HELLO) {
    const lc_hello_t *x = &a->u.hello, *y = &b->u.hello;
    if (strcmp(x->name, y->name) || x->n_zones != y->n_zones || x->caps != y->caps || x->challenge != y->challenge ||
        x->echo != y->echo || x->bye != y->bye)
      return false;
    for (int i = 0; i < x->n_zones; i++)
      if (strcmp(x->zones[i], y->zones[i]))
        return false;
    return true;
  }
  if (a->type == LC_MSG_TALK_START)
    return target_eq(&a->u.talk_start.target, &b->u.talk_start.target) &&
           a->u.talk_start.stream_id == b->u.talk_start.stream_id;
  return a->u.talk_stop.stream_id == b->u.talk_stop.stream_id;
}

static void test_control(void) {
  for (size_t i = 0; i < N(CONTROL_VECS); i++) {
    const control_vec_t *v = &CONTROL_VECS[i];
    uint8_t bytes[256], enc[256];
    size_t len = hex_decode(v->bytes, bytes, sizeof(bytes));
    lc_control_t msg;
    CHECK(lc_control_decode(bytes, len, &msg) == 0);
    if (!control_eq(&msg, &v->msg))
      fprintf(stderr, "control vector %s: decode mismatch\n", v->name);
    CHECK(control_eq(&msg, &v->msg));
    if (!v->decode_only) {
      int n = lc_control_encode(&v->msg, enc, sizeof(enc));
      CHECK_EQ(n, len);
      CHECK(n == (int)len && memcmp(enc, bytes, len) == 0);
    }
  }
  /* Truncated or garbage input never crashes and is mostly rejected. */
  uint8_t junk[64];
  unsigned seed = 1;
  for (int round = 0; round < 20000; round++) {
    size_t len = (size_t)(rand_r(&seed) % sizeof(junk));
    for (size_t k = 0; k < len; k++)
      junk[k] = (uint8_t)rand_r(&seed);
    lc_control_t msg;
    (void)lc_control_decode(junk, len, &msg);
  }
  /* Long submessage (> 127 bytes) exercises the multi-byte length path. */
  lc_control_t big = {.type = LC_MSG_HELLO};
  memset(big.u.hello.name, 'n', LC_NAME_MAX);
  for (int z = 0; z < LC_ZONES_MAX; z++) {
    memset(big.u.hello.zones[z], 'a' + z, LC_ZONE_MAX);
  }
  big.u.hello.n_zones = LC_ZONES_MAX;
  big.u.hello.challenge = 5;
  uint8_t buf[512];
  int n = lc_control_encode(&big, buf, sizeof(buf));
  CHECK(n > 130);
  lc_control_t back;
  CHECK(lc_control_decode(buf, (size_t)n, &back) == 0);
  CHECK(control_eq(&big, &back));
  CHECK(lc_control_encode(&big, buf, 100) == -1);
}

static void test_replay(void) {
  for (size_t i = 0; i < N(REPLAY_VECS); i++) {
    const replay_vec_t *v = &REPLAY_VECS[i];
    lc_replay_t w;
    lc_replay_init(&w, v->first);
    for (size_t s = 0; s < v->n; s++)
      CHECK(lc_replay_check(&w, v->seq[s]) == v->ok[s]);
  }
}

static void test_targets(void) {
  lc_target_t t;
  CHECK(lc_target_parse("all", &t) == 0 && t.kind == LC_TARGET_ALL);
  CHECK(lc_target_parse("zone:kids", &t) == 0 && t.kind == LC_TARGET_ZONE && strcmp(t.zone, "kids") == 0);
  CHECK(lc_target_parse("device:8a3f01c2", &t) == 0 && t.kind == LC_TARGET_DEVICE && t.device == 0x8a3f01c2u);
  CHECK(lc_target_parse("device:xyz", &t) != 0);
  CHECK(lc_target_parse("bogus", &t) != 0);
}

void test_vectors(void) {
  test_keys();
  test_packets();
  test_invalid();
  test_control();
  test_replay();
  test_targets();
}
