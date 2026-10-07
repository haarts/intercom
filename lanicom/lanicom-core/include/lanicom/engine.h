/* The protocol engine: discovery, epoch verification, replay protection, talk.
 *
 * No sockets, threads or clocks: the caller feeds received datagrams and the
 * time, and gets packets to send and events through callbacks. Not thread-safe;
 * serialise calls. Same behaviour as python/src/lanicom/node.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "control.h"
#include "crypto.h"
#include "packet.h"
#include "replay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LC_PORT 47100
#define LC_MULTICAST_IP 0xEFFF4C49u /* 239.255.76.73 */
#define LC_BROADCAST_IP 0xFFFFFFFFu
#define LC_MAX_PEERS 32
#define LC_MAX_STATIC 8
#define LC_HELLO_INTERVAL_MS 5000
#define LC_HELLO_JITTER_MS 1000
#define LC_PEER_TIMEOUT_MS 30000
#define LC_CHALLENGE_INTERVAL_MS 250
#define LC_ECHO_LIMIT_PER_S 10

typedef struct {
  uint32_t ip; /* host byte order, e.g. 0xC0A80101 = 192.168.1.1 */
  uint16_t port;
} lc_addr_t;

typedef struct {
  bool in_use;
  uint32_t sender_id;
  lc_addr_t addr;
  uint32_t first_seen_ms, last_seen_ms;
  lc_hello_t info; /* name, caps from the last verified Hello */
  bool announced;  /* peer_added fired */
  bool verified;
  uint64_t verified_epoch;
  lc_replay_t window;
  uint64_t challenge;
  uint32_t challenge_sent_ms;
  bool challenge_ever_sent;
  int32_t rtt_ms; /* -1 until measured */
  uint8_t send_key[32];
} lc_peer_t;

typedef struct {
  uint32_t malformed, foreign, auth, unverified, replay, collision, table_full;
  uint32_t rx_control, rx_audio, tx, verified;
} lc_stats_t;

typedef struct {
  void (*send)(void *ctx, lc_addr_t to, const uint8_t *data, size_t len);
  uint32_t (*random32)(void *ctx);
  /* All optional: */
  void (*peer_added)(void *ctx, const lc_peer_t *peer);
  void (*peer_removed)(void *ctx, const lc_peer_t *peer);
  void (*peer_updated)(void *ctx, const lc_peer_t *peer);
  void (*talk_start)(void *ctx, const lc_peer_t *peer, const lc_target_t *target, uint32_t stream_id);
  void (*talk_stop)(void *ctx, const lc_peer_t *peer, uint32_t stream_id);
  void (*audio)(void *ctx, const lc_peer_t *peer, uint32_t stream_id, uint32_t ts, const uint8_t *opus,
                size_t len);
  void (*sender_id_changed)(void *ctx, uint32_t sender_id); /* persist it */
  /* Every verified Hello (after peer_added/peer_updated), e.g. to check the peer's links. */
  void (*hello)(void *ctx, const lc_peer_t *peer, const lc_hello_t *hello);
  /* Verified pairing messages (LC_MSG_PAIR_*). */
  void (*control)(void *ctx, const lc_peer_t *peer, const lc_control_t *msg);
  void *ctx;
} lc_callbacks_t;

typedef struct {
  uint32_t sender_id, stream_id;
  uint32_t at_ms;
  bool stopped;
} lc_talk_seen_t;

typedef struct {
  uint32_t ip, window_ms;
  uint8_t count;
} lc_echo_budget_t;

typedef struct {
  const lc_key_t *key;
  lc_callbacks_t cb;
  uint32_t sender_id;
  uint64_t epoch;
  uint32_t seq;
  bool seq_wrapped;
  lc_hello_t self;
  uint16_t port;
  bool broadcast, multicast, rtt_probe;
  lc_addr_t statics[LC_MAX_STATIC];
  uint8_t n_static;
  lc_peer_t peers[LC_MAX_PEERS];
  uint32_t next_hello_ms, next_expiry_ms;
  uint8_t startup_hellos;
  lc_talk_seen_t talks[16];
  uint8_t talk_next;
  lc_echo_budget_t echo[8];
  lc_stats_t stats;
  uint8_t tx[LC_MAX_PACKET];
  uint8_t rx_plain[LC_MAX_PACKET];
} lc_engine_t;

#define LC_TALK_MAX_DEVICES 8

/* One outgoing stream: to every peer (target all), or to a set of devices. A device set gets
 * one stream, sealed once per frame; each device gets its own TalkStart naming it. */
typedef struct {
  lc_target_t target; /* LC_TARGET_DEVICE: the set is devices[] */
  uint32_t devices[LC_TALK_MAX_DEVICES];
  uint8_t starts[LC_TALK_MAX_DEVICES]; /* TalkStarts sent to each device so far */
  uint8_t n_devices;
  uint32_t stream_id;
  uint32_t ts;
  uint32_t frames;
  bool active;
} lc_talk_t;

/* `epoch` must never repeat for this sender_id: use (boot_counter << 32) | random32. */
void lc_engine_init(lc_engine_t *e, const lc_key_t *key, uint32_t sender_id, uint64_t epoch,
                    const lc_callbacks_t *cb, uint32_t now_ms);
/* Announces the change to peers. */
void lc_engine_set_identity(lc_engine_t *e, const char *name, uint32_t caps);
/* Our button links, listed in every Hello. Announces the change to peers. */
void lc_engine_set_links(lc_engine_t *e, const lc_link_t *links, size_t n);
/* Unicast a control message to a verified peer; -1 if there is no such peer. */
int lc_engine_send_control(lc_engine_t *e, uint32_t sender_id, const lc_control_t *msg);
/* Broadcast a control message (and send it to the static peers). */
void lc_engine_broadcast_control(lc_engine_t *e, const lc_control_t *msg);
uint64_t lc_engine_random64(lc_engine_t *e); /* non-zero */
int lc_engine_add_static_peer(lc_engine_t *e, lc_addr_t addr);
void lc_engine_receive(lc_engine_t *e, const uint8_t *data, size_t len, lc_addr_t from, uint32_t now_ms);
/* Call every 100 ms or so: Hellos, expiry. */
void lc_engine_tick(lc_engine_t *e, uint32_t now_ms);
/* Clean shutdown: tells peers to forget us. */
void lc_engine_bye(lc_engine_t *e);

const lc_peer_t *lc_engine_find_peer(const lc_engine_t *e, uint32_t sender_id);
size_t lc_engine_peer_count(const lc_engine_t *e); /* verified peers */
size_t lc_engine_recipient_count(const lc_engine_t *e, const lc_target_t *target);

void lc_talk_begin(lc_engine_t *e, lc_talk_t *talk, const lc_target_t *target);
/* A stream to a set of devices (at most LC_TALK_MAX_DEVICES; duplicates are ignored). */
void lc_talk_begin_devices(lc_engine_t *e, lc_talk_t *talk, const uint32_t *devices, size_t n);
/* Change the set mid-stream: new devices get TalkStarts, dropped ones a TalkStop. */
void lc_talk_set_devices(lc_engine_t *e, lc_talk_t *talk, const uint32_t *devices, size_t n);
/* One Opus frame of `samples` (16 kHz). Returns the number of recipients. */
int lc_talk_frame(lc_engine_t *e, lc_talk_t *talk, const uint8_t *opus, size_t len, uint32_t samples);
void lc_talk_end(lc_engine_t *e, lc_talk_t *talk);

#ifdef __cplusplus
}
#endif
