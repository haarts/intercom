#include "test.h"

#include "lanicom/jitter.h"

/* Fake frames: the payload is the frame index; 10 ms = 480 ticks. */
static void push(lc_jitter_t *j, uint32_t base, int i) {
  uint8_t data[2] = {(uint8_t)i, 0xAA};
  lc_jitter_push(j, base + (uint32_t)i * 480, data, sizeof(data), 480);
}

static void test_order_and_start(void) {
  lc_jitter_t j;
  lc_jitter_init(&j, 20, 60);
  lc_jb_out_t o;
  push(&j, 1000, 1);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_NONE); /* still buffering */
  push(&j, 1000, 0);                           /* reordered */
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FRAME && o.data[0] == 0 && o.samples == 160);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FRAME && o.data[0] == 1);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_NONE);
}

static void test_fec_and_plc(void) {
  lc_jitter_t j;
  lc_jitter_init(&j, 20, 60);
  lc_jb_out_t o;
  push(&j, 0xFFFFFF00u, 0); /* ts wraps during the test */
  push(&j, 0xFFFFFF00u, 2);
  push(&j, 0xFFFFFF00u, 3);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FRAME && o.data[0] == 0);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FEC && o.data[0] == 2 && o.samples == 160);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FRAME && o.data[0] == 2);
  push(&j, 0xFFFFFF00u, 5);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FRAME && o.data[0] == 3);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FEC && o.data[0] == 5); /* 4 lost */
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_FRAME && o.data[0] == 5);
  push(&j, 0xFFFFFF00u, 8);
  CHECK(lc_jitter_pop(&j, &o) == LC_JB_PLC); /* 6 lost, 7 not here either */
  CHECK_EQ(j.stats.lost, 3);
  CHECK_EQ(j.stats.fec, 2);
}

static void test_late_grows_target(void) {
  lc_jitter_t j;
  lc_jitter_init(&j, 20, 60);
  lc_jb_out_t o;
  push(&j, 0, 0);
  push(&j, 0, 1);
  push(&j, 0, 3);
  lc_jitter_pop(&j, &o);
  lc_jitter_pop(&j, &o);
  lc_jitter_pop(&j, &o); /* 2 concealed via FEC */
  push(&j, 0, 2);        /* too late */
  CHECK_EQ(j.stats.late, 1);
  CHECK_EQ(j.target, 30 * 48);
  for (int i = 0; i < 10; i++)
    push(&j, 0, 100 + i * 0); /* duplicates of one frame */
  CHECK_EQ(j.stats.duplicate, 9);
}

static void test_bounded_depth(void) {
  lc_jitter_t j;
  lc_jitter_init(&j, 20, 60);
  lc_jb_out_t o;
  push(&j, 0, 0);
  push(&j, 0, 1);
  lc_jitter_pop(&j, &o);
  for (int i = 2; i < 40; i++) {
    push(&j, 0, i);
    CHECK(lc_jitter_depth(&j) <= 60 * 48);
  }
  CHECK(j.stats.dropped_for_latency > 0);
}

/* --- mixer with a fake decoder: a FRAME decodes to `samples` copies of data[0] * 100. */

static int decoded[LC_RX_STREAMS];
static int begun, ended;

static int fake_decode(void *ctx, int slot, lc_jb_kind_t kind, const uint8_t *data, size_t len, uint32_t samples,
                       int16_t *pcm) {
  (void)ctx;
  (void)len;
  decoded[slot]++;
  int16_t v = kind == LC_JB_FRAME ? (int16_t)(data[0] * 100) : 0;
  for (uint32_t i = 0; i < samples; i++)
    pcm[i] = v;
  return (int)samples;
}
static void on_begin(void *ctx, int slot, uint32_t s, uint32_t st) { (void)ctx, (void)slot, (void)s, (void)st, begun++; }
static void on_end(void *ctx, int slot, uint32_t s, uint32_t st) { (void)ctx, (void)slot, (void)s, (void)st, ended++; }

static void test_mixer(void) {
  static lc_rx_t rx;
  lc_rx_init(&rx, 20, 60, fake_decode, on_begin, on_end, NULL);
  uint8_t a[1] = {100}, b[1] = {200}; /* 10000 + 20000 -> 30000 */
  for (int i = 0; i < 3; i++) {
    lc_rx_push(&rx, 1, 11, (uint32_t)i * 480, a, 1, 480, 0);
    lc_rx_push(&rx, 2, 22, 5000 + (uint32_t)i * 480, b, 1, 480, 0);
  }
  CHECK_EQ(begun, 2);
  CHECK_EQ(lc_rx_active(&rx), 2);
  int16_t out[160];
  CHECK_EQ(lc_rx_read(&rx, out, 160, 10), 2);
  CHECK_EQ(out[0], 30000);
  CHECK_EQ(out[159], 30000);
  uint8_t loud[1] = {250}; /* saturates: 25000 + 20000 */
  lc_rx_push(&rx, 1, 11, 3 * 480, loud, 1, 480, 10);
  lc_rx_push(&rx, 2, 22, 5000 + 3 * 480, b, 1, 480, 10);
  lc_rx_read(&rx, out, 160, 20); /* frame 1 */
  lc_rx_read(&rx, out, 160, 30); /* frame 2 */
  CHECK_EQ(out[0], 30000);
  lc_rx_read(&rx, out, 160, 40); /* frame 3 */
  CHECK_EQ(out[0], 32767);
  lc_rx_stop(&rx, 2, 22);
  for (int t = 40; t < 1000; t += 10)
    lc_rx_read(&rx, out, 160, (uint32_t)t);
  CHECK_EQ(ended, 2);
  CHECK_EQ(lc_rx_active(&rx), 0);
  CHECK_EQ(out[0], 0);
  /* All slots busy: a fifth stream is dropped. */
  for (uint32_t s = 0; s < LC_RX_STREAMS; s++)
    CHECK(lc_rx_push(&rx, 10 + s, 1, 0, a, 1, 480, 1000) >= 0);
  CHECK(lc_rx_push(&rx, 99, 1, 0, a, 1, 480, 1000) == -1);
  CHECK_EQ(rx.busy_drops, 1);
}

void test_jitter(void) {
  test_order_and_start();
  test_fec_and_plc();
  test_late_grows_target();
  test_bounded_depth();
  test_mixer();
}
