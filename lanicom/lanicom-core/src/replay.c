#include "lanicom/replay.h"

void lc_replay_init(lc_replay_t *w, uint32_t first_seq) {
  w->highest = first_seq;
  w->bitmap = 1;
}

bool lc_replay_check(lc_replay_t *w, uint32_t seq) {
  if (seq > w->highest) {
    uint32_t shift = seq - w->highest;
    w->bitmap = shift < 64 ? (w->bitmap << shift) | 1 : 1;
    w->highest = seq;
    return true;
  }
  uint32_t offset = w->highest - seq;
  if (offset >= 64 || (w->bitmap >> offset & 1))
    return false;
  w->bitmap |= (uint64_t)1 << offset;
  return true;
}
