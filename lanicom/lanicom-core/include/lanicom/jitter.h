/* Adaptive jitter buffer and the receive mixer.
 *
 * Frames are ordered by the payload ts (48 kHz ticks), never by packet seq.
 * Decoding is delegated to a callback, so this stays codec-library-free and
 * host-testable. Same algorithm as python/src/lanicom/jitter.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_JB_SLOTS 16
#define LC_JB_FRAME_MAX 256
#define LC_TICKS_PER_SAMPLE 3 /* 48 kHz ticks per 16 kHz sample */

typedef struct {
  bool used;
  uint32_t ts;
  uint16_t ticks;
  uint16_t len;
  uint8_t data[LC_JB_FRAME_MAX];
} lc_jb_slot_t;

typedef struct {
  uint32_t received, late, duplicate, lost, fec, dropped_for_latency, overflow;
} lc_jb_stats_t;

typedef struct {
  lc_jb_slot_t slots[LC_JB_SLOTS];
  bool started;
  uint32_t next_ts;
  uint32_t last_ticks;
  uint32_t target, max, step; /* ticks */
  uint32_t over_target;
  lc_jb_stats_t stats;
} lc_jitter_t;

typedef enum { LC_JB_NONE = 0, LC_JB_FRAME, LC_JB_FEC, LC_JB_PLC } lc_jb_kind_t;

typedef struct {
  lc_jb_kind_t kind;
  const uint8_t *data; /* FRAME: the frame; FEC: the *next* frame. Valid until the next push/pop. */
  size_t len;
  uint32_t samples; /* 16 kHz samples this item produces */
} lc_jb_out_t;

static inline int32_t lc_ts_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void lc_jitter_init(lc_jitter_t *j, uint32_t target_ms, uint32_t max_ms);
/* `ticks`: the frame's duration in 48 kHz ticks (opus_packet_get_nb_samples at 16 kHz * 3). */
void lc_jitter_push(lc_jitter_t *j, uint32_t ts, const uint8_t *data, size_t len, uint32_t ticks);
lc_jb_kind_t lc_jitter_pop(lc_jitter_t *j, lc_jb_out_t *out);
uint32_t lc_jitter_depth(const lc_jitter_t *j);
bool lc_jitter_empty(const lc_jitter_t *j);

/* --- receive mixer ------------------------------------------------------ */

#define LC_RX_STREAMS 4
#define LC_RX_PCM_MAX 1920 /* 120 ms at 16 kHz */
#define LC_STREAM_TIMEOUT_MS 300
#define LC_CONCEAL_MS 60

/* Decode one item for stream `slot` into `pcm` (room for LC_RX_PCM_MAX samples).
 * FRAME: decode `data`. FEC: decode the FEC data in `data` (opus decode_fec=1) for
 * `samples`. PLC: conceal `samples`. Returns the number of samples written, or < 0. */
typedef int (*lc_decode_fn)(void *ctx, int slot, lc_jb_kind_t kind, const uint8_t *data, size_t len,
                            uint32_t samples, int16_t *pcm);
/* Called when a slot starts a new stream (reset that slot's decoder) or ends one. */
typedef void (*lc_stream_fn)(void *ctx, int slot, uint32_t sender_id, uint32_t stream_id);

typedef struct {
  bool active, stopped;
  uint32_t sender_id, stream_id;
  uint32_t last_packet_ms;
  lc_jitter_t jb;
  int16_t pcm[LC_RX_PCM_MAX];
  uint32_t pcm_len;
} lc_rx_stream_t;

typedef struct {
  lc_rx_stream_t streams[LC_RX_STREAMS];
  uint32_t target_ms, max_ms;
  lc_decode_fn decode;
  lc_stream_fn stream_begin, stream_end;
  void *ctx;
  uint32_t busy_drops;
  int16_t scratch[LC_RX_PCM_MAX];
} lc_rx_t;

void lc_rx_init(lc_rx_t *rx, uint32_t target_ms, uint32_t max_ms, lc_decode_fn decode, lc_stream_fn begin,
                lc_stream_fn end, void *ctx);
/* Returns the stream slot, or -1 if all slots are busy (frame dropped). */
int lc_rx_push(lc_rx_t *rx, uint32_t sender_id, uint32_t stream_id, uint32_t ts, const uint8_t *data, size_t len,
               uint32_t ticks, uint32_t now_ms);
void lc_rx_stop(lc_rx_t *rx, uint32_t sender_id, uint32_t stream_id);
/* Mix `n` samples into `out` (always fully written; silence when idle). Returns the number
 * of streams that contributed audio. Ends finished streams. */
int lc_rx_read(lc_rx_t *rx, int16_t *out, size_t n, uint32_t now_ms);
int lc_rx_active(const lc_rx_t *rx);

#ifdef __cplusplus
}
#endif
