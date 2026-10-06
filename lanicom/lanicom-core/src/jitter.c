#include "lanicom/jitter.h"

#include <string.h>

#define MS_TO_TICKS(ms) ((ms) * 48u)

void lc_jitter_init(lc_jitter_t *j, uint32_t target_ms, uint32_t max_ms) {
  memset(j, 0, sizeof(*j));
  j->target = MS_TO_TICKS(target_ms);
  j->max = MS_TO_TICKS(max_ms);
  j->step = MS_TO_TICKS(10);
  j->last_ticks = 480;
}

bool lc_jitter_empty(const lc_jitter_t *j) {
  for (int i = 0; i < LC_JB_SLOTS; i++)
    if (j->slots[i].used)
      return false;
  return true;
}

static int find(const lc_jitter_t *j, uint32_t ts) {
  for (int i = 0; i < LC_JB_SLOTS; i++)
    if (j->slots[i].used && j->slots[i].ts == ts)
      return i;
  return -1;
}

/* Oldest buffered frame (relative to an arbitrary buffered reference). */
static int oldest(const lc_jitter_t *j) {
  int best = -1;
  for (int i = 0; i < LC_JB_SLOTS; i++) {
    if (!j->slots[i].used)
      continue;
    if (best < 0 || lc_ts_diff(j->slots[i].ts, j->slots[best].ts) < 0)
      best = i;
  }
  return best;
}

uint32_t lc_jitter_depth(const lc_jitter_t *j) {
  int o = oldest(j);
  if (o < 0)
    return 0;
  uint32_t start = j->started ? j->next_ts : j->slots[o].ts;
  int32_t end = 0;
  for (int i = 0; i < LC_JB_SLOTS; i++) {
    if (!j->slots[i].used)
      continue;
    int32_t e = lc_ts_diff(j->slots[i].ts, start) + j->slots[i].ticks;
    if (e > end)
      end = e;
  }
  return (uint32_t)end;
}

static void skip(lc_jitter_t *j) {
  int i = find(j, j->next_ts);
  uint32_t ticks = j->last_ticks;
  if (i >= 0) {
    ticks = j->slots[i].ticks;
    j->slots[i].used = false;
  }
  j->next_ts += ticks;
  j->stats.dropped_for_latency++;
}

void lc_jitter_push(lc_jitter_t *j, uint32_t ts, const uint8_t *data, size_t len, uint32_t ticks) {
  j->stats.received++;
  if (len == 0 || len > LC_JB_FRAME_MAX || ticks == 0 || ticks > 0xFFFF) {
    j->stats.overflow++;
    return;
  }
  if (j->started && lc_ts_diff(ts, j->next_ts) < 0) {
    j->stats.late++;
    j->target = j->target + j->step < j->max ? j->target + j->step : j->max;
    return;
  }
  if (find(j, ts) >= 0) {
    j->stats.duplicate++;
    return;
  }
  int slot = -1;
  for (int i = 0; i < LC_JB_SLOTS && slot < 0; i++)
    if (!j->slots[i].used)
      slot = i;
  if (slot < 0) {
    /* Full: make room by dropping the oldest frame. */
    slot = oldest(j);
    if (j->started)
      j->next_ts = j->slots[slot].ts + j->slots[slot].ticks;
    j->stats.overflow++;
  }
  lc_jb_slot_t *s = &j->slots[slot];
  s->used = true;
  s->ts = ts;
  s->ticks = (uint16_t)ticks;
  s->len = (uint16_t)len;
  memcpy(s->data, data, len);
  while (j->started && lc_jitter_depth(j) > j->max)
    skip(j);
}

lc_jb_kind_t lc_jitter_pop(lc_jitter_t *j, lc_jb_out_t *out) {
  memset(out, 0, sizeof(*out));
  if (!j->started) {
    int o = oldest(j);
    if (o < 0 || lc_jitter_depth(j) < j->target)
      return LC_JB_NONE;
    j->started = true;
    j->next_ts = j->slots[o].ts;
  }
  if (lc_jitter_empty(j))
    return LC_JB_NONE;
  /* Drift control: sitting well above target for a while drops a frame. */
  if (lc_jitter_depth(j) > j->target + 2 * j->last_ticks) {
    if (++j->over_target >= 50) {
      j->over_target = 0;
      skip(j);
      if (lc_jitter_empty(j))
        return LC_JB_NONE;
    }
  } else {
    j->over_target = 0;
  }
  int i = find(j, j->next_ts);
  if (i >= 0) {
    lc_jb_slot_t *s = &j->slots[i];
    s->used = false; /* data stays valid until the next push */
    j->next_ts += s->ticks;
    j->last_ticks = s->ticks;
    out->kind = LC_JB_FRAME;
    out->data = s->data;
    out->len = s->len;
    out->samples = s->ticks / LC_TICKS_PER_SAMPLE;
    return out->kind;
  }
  int32_t gap = INT32_MAX;
  for (int k = 0; k < LC_JB_SLOTS; k++) {
    if (j->slots[k].used) {
      int32_t d = lc_ts_diff(j->slots[k].ts, j->next_ts);
      if (d < gap)
        gap = d;
    }
  }
  if (gap >= (int32_t)j->max) {
    /* The sender jumped (e.g. restarted its ts): resync instead of concealing for ages. */
    j->next_ts = j->slots[oldest(j)].ts;
    return lc_jitter_pop(j, out);
  }
  uint32_t ticks = j->last_ticks;
  j->stats.lost++;
  j->next_ts += ticks;
  out->samples = ticks / LC_TICKS_PER_SAMPLE;
  int n = find(j, j->next_ts);
  if (n >= 0) {
    j->stats.fec++;
    out->kind = LC_JB_FEC;
    out->data = j->slots[n].data;
    out->len = j->slots[n].len;
    return out->kind;
  }
  out->kind = LC_JB_PLC;
  return out->kind;
}

/* --- receive mixer ------------------------------------------------------ */

void lc_rx_init(lc_rx_t *rx, uint32_t target_ms, uint32_t max_ms, lc_decode_fn decode, lc_stream_fn begin,
                lc_stream_fn end, void *ctx) {
  memset(rx, 0, sizeof(*rx));
  rx->target_ms = target_ms;
  rx->max_ms = max_ms;
  rx->decode = decode;
  rx->stream_begin = begin;
  rx->stream_end = end;
  rx->ctx = ctx;
}

int lc_rx_push(lc_rx_t *rx, uint32_t sender_id, uint32_t stream_id, uint32_t ts, const uint8_t *data, size_t len,
               uint32_t ticks, uint32_t now_ms) {
  int slot = -1, free_slot = -1;
  for (int i = 0; i < LC_RX_STREAMS; i++) {
    lc_rx_stream_t *s = &rx->streams[i];
    if (s->active && s->sender_id == sender_id && s->stream_id == stream_id)
      slot = i;
    else if (!s->active && free_slot < 0)
      free_slot = i;
  }
  if (slot < 0) {
    if (free_slot < 0) {
      rx->busy_drops++;
      return -1;
    }
    slot = free_slot;
    lc_rx_stream_t *s = &rx->streams[slot];
    s->active = true;
    s->stopped = false;
    s->sender_id = sender_id;
    s->stream_id = stream_id;
    s->pcm_len = 0;
    lc_jitter_init(&s->jb, rx->target_ms, rx->max_ms);
    if (rx->stream_begin)
      rx->stream_begin(rx->ctx, slot, sender_id, stream_id);
  }
  lc_rx_stream_t *s = &rx->streams[slot];
  s->last_packet_ms = now_ms;
  lc_jitter_push(&s->jb, ts, data, len, ticks);
  return slot;
}

void lc_rx_stop(lc_rx_t *rx, uint32_t sender_id, uint32_t stream_id) {
  for (int i = 0; i < LC_RX_STREAMS; i++) {
    lc_rx_stream_t *s = &rx->streams[i];
    if (s->active && s->sender_id == sender_id && s->stream_id == stream_id)
      s->stopped = true;
  }
}

int lc_rx_active(const lc_rx_t *rx) {
  int n = 0;
  for (int i = 0; i < LC_RX_STREAMS; i++)
    n += rx->streams[i].active;
  return n;
}

/* Fill the stream's PCM FIFO to at least `n` samples if it can. */
static void fill(lc_rx_t *rx, int slot, size_t n, uint32_t now_ms) {
  lc_rx_stream_t *s = &rx->streams[slot];
  while (s->pcm_len < n) {
    lc_jb_out_t item;
    if (lc_jitter_pop(&s->jb, &item) == LC_JB_NONE) {
      if (!s->jb.started || s->stopped || (int32_t)(now_ms - s->last_packet_ms) > LC_CONCEAL_MS)
        return;
      item.kind = LC_JB_PLC; /* underrun mid-stream */
      item.samples = s->jb.last_ticks / LC_TICKS_PER_SAMPLE;
    }
    int16_t *tmp = rx->scratch;
    int got = rx->decode(rx->ctx, slot, item.kind, item.data, item.len, item.samples, tmp);
    if (got <= 0)
      continue; /* undecodable frame: skip it */
    size_t room = LC_RX_PCM_MAX - s->pcm_len;
    size_t take = (size_t)got < room ? (size_t)got : room;
    memcpy(s->pcm + s->pcm_len, tmp, take * sizeof(int16_t));
    s->pcm_len += (uint32_t)take;
  }
}

int lc_rx_read(lc_rx_t *rx, int16_t *out, size_t n, uint32_t now_ms) {
  int32_t acc[480];
  int contributing = 0;
  size_t done = 0;
  while (done < n) {
    size_t chunk = n - done < 480 ? n - done : 480;
    memset(acc, 0, sizeof(acc));
    for (int i = 0; i < LC_RX_STREAMS; i++) {
      lc_rx_stream_t *s = &rx->streams[i];
      if (!s->active)
        continue;
      fill(rx, i, chunk, now_ms);
      size_t take = s->pcm_len < chunk ? s->pcm_len : chunk;
      for (size_t k = 0; k < take; k++)
        acc[k] += s->pcm[k];
      memmove(s->pcm, s->pcm + take, (s->pcm_len - take) * sizeof(int16_t));
      s->pcm_len -= (uint32_t)take;
      if (take && done == 0)
        contributing++;
    }
    for (size_t k = 0; k < chunk; k++)
      out[done + k] = (int16_t)(acc[k] > 32767 ? 32767 : acc[k] < -32768 ? -32768 : acc[k]);
    done += chunk;
  }
  for (int i = 0; i < LC_RX_STREAMS; i++) {
    lc_rx_stream_t *s = &rx->streams[i];
    if (!s->active)
      continue;
    bool idle = s->stopped || (int32_t)(now_ms - s->last_packet_ms) > LC_STREAM_TIMEOUT_MS;
    if (idle && lc_jitter_empty(&s->jb) && s->pcm_len == 0) {
      s->active = false;
      if (rx->stream_end)
        rx->stream_end(rx->ctx, i, s->sender_id, s->stream_id);
    }
  }
  return contributing;
}
