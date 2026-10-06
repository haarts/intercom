/* 64-packet sliding replay window (spec/PROTOCOL.md section 4). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t highest;
  uint64_t bitmap; /* bit i set: highest - i seen */
} lc_replay_t;

void lc_replay_init(lc_replay_t *w, uint32_t first_seq);
/* True if seq is new (and records it); false for duplicates and too-old packets. */
bool lc_replay_check(lc_replay_t *w, uint32_t seq);

#ifdef __cplusplus
}
#endif
