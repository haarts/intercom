/* Control messages (spec/lanicom.proto), hand-coded protobuf wire format. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_CAP_PLAYBACK 1u
#define LC_CAP_CAPTURE 2u
#define LC_CAP_MONITOR 4u /* wants TalkStart/TalkStop for every stream, no audio */
#define LC_NAME_MAX 32
#define LC_ZONE_MAX 16
#define LC_ZONES_MAX 8

typedef enum { LC_TARGET_ALL = 0, LC_TARGET_DEVICE, LC_TARGET_ZONE } lc_target_kind_t;

typedef struct {
  lc_target_kind_t kind;
  uint32_t device;
  char zone[LC_ZONE_MAX + 1];
} lc_target_t;

typedef struct {
  char name[LC_NAME_MAX + 1];
  char zones[LC_ZONES_MAX][LC_ZONE_MAX + 1];
  uint8_t n_zones;
  uint32_t caps;
  uint64_t challenge;
  uint64_t echo;
  bool bye;
} lc_hello_t;

typedef enum { LC_MSG_NONE = 0, LC_MSG_HELLO, LC_MSG_TALK_START, LC_MSG_TALK_STOP } lc_msg_type_t;

typedef struct {
  lc_msg_type_t type;
  union {
    lc_hello_t hello;
    struct {
      lc_target_t target;
      uint32_t stream_id;
    } talk_start;
    struct {
      uint32_t stream_id;
    } talk_stop;
  } u;
} lc_control_t;

/* Returns the encoded length, or -1 if it doesn't fit in `cap`. */
int lc_control_encode(const lc_control_t *msg, uint8_t *out, size_t cap);
/* Returns 0 on success (msg->type may be LC_MSG_NONE for an unknown message), -1 if malformed.
 * Strings longer than our limits are truncated; zones beyond LC_ZONES_MAX are ignored. */
int lc_control_decode(const uint8_t *data, size_t len, lc_control_t *msg);

bool lc_target_matches(const lc_target_t *t, uint32_t sender_id, const lc_hello_t *hello);
/* "all", "zone:<z>" or "device:<8 hex>"; 0 on success. */
int lc_target_parse(const char *text, lc_target_t *t);

#ifdef __cplusplus
}
#endif
