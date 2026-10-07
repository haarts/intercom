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
#define LC_CAP_ANNOUNCER 8u /* may talk to any device, partner or not (Home Assistant) */
#define LC_NAME_MAX 32
#define LC_MAX_LINKS 8 /* links kept from a Hello; more are skipped */

typedef enum { LC_TARGET_ALL = 0, LC_TARGET_DEVICE } lc_target_kind_t;

typedef struct {
  lc_target_kind_t kind;
  uint32_t device;
} lc_target_t;

/* One of the sender's buttons linked to a button on another device; buttons are 1-based. */
typedef struct {
  uint32_t button;
  uint32_t partner; /* sender_id */
  uint32_t partner_button;
} lc_link_t;

typedef struct {
  char name[LC_NAME_MAX + 1];
  uint32_t caps;
  uint64_t challenge;
  uint64_t echo;
  bool bye;
  uint8_t n_links;
  lc_link_t links[LC_MAX_LINKS];
} lc_hello_t;

typedef enum {
  LC_MSG_NONE = 0,
  LC_MSG_HELLO,
  LC_MSG_TALK_START,
  LC_MSG_TALK_STOP,
  LC_MSG_PAIR_OFFER,
  LC_MSG_PAIR_ACCEPT,
  LC_MSG_PAIR_CONFIRM,
} lc_msg_type_t;

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
    struct {
      uint64_t nonce;
      uint32_t button; /* not sent in PairConfirm */
    } pair;              /* PairOffer, PairAccept, PairConfirm */
  } u;
} lc_control_t;

/* Returns the encoded length, or -1 if it doesn't fit in `cap`. */
int lc_control_encode(const lc_control_t *msg, uint8_t *out, size_t cap);
/* Returns 0 on success (msg->type may be LC_MSG_NONE for an unknown message), -1 if malformed.
 * Strings longer than our limits are truncated. */
int lc_control_decode(const uint8_t *data, size_t len, lc_control_t *msg);

bool lc_target_matches(const lc_target_t *t, uint32_t sender_id);
/* "all" or "device:<8 hex>"; 0 on success. */
int lc_target_parse(const char *text, lc_target_t *t);

#ifdef __cplusplus
}
#endif
