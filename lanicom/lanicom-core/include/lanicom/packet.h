/* Packet header (spec/PROTOCOL.md section 3). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LC_VERSION 1
#define LC_TYPE_CONTROL 1
#define LC_TYPE_AUDIO 2
#define LC_HEADER_LEN 20
#define LC_TAG_LEN 16
#define LC_MIN_PACKET (LC_HEADER_LEN + LC_TAG_LEN)
#define LC_MAX_PACKET 1200
#define LC_MAX_PLAINTEXT (LC_MAX_PACKET - LC_MIN_PACKET)

typedef enum {
  LC_OK = 0,
  LC_ERR_MALFORMED = -1,
  LC_ERR_FOREIGN = -2,
  LC_ERR_AUTH = -3,
  LC_ERR_TOO_LARGE = -4,
  LC_ERR_CRYPTO = -5,
} lc_err_t;

typedef struct {
  uint8_t version;
  uint8_t type;
  uint16_t key_id;
  uint32_t sender_id;
  uint64_t epoch;
  uint32_t seq;
} lc_header_t;

void lc_header_pack(const lc_header_t *h, uint8_t out[LC_HEADER_LEN]);
/* Checks length, version and type only. */
lc_err_t lc_header_parse(const uint8_t *data, size_t len, lc_header_t *h);

/* Seal into `out` (capacity `cap`); returns the packet length or a negative lc_err_t. */
int lc_seal(const uint8_t send_key[32], const lc_header_t *h, const uint8_t *plaintext, size_t len,
            uint8_t *out, size_t cap);
/* Authenticate and decrypt; `plaintext` needs len - 36 bytes. Returns the plaintext
 * length or a negative lc_err_t. Does not check key_id (the caller picks the key). */
int lc_open(const uint8_t send_key[32], const uint8_t *data, size_t len, uint8_t *plaintext);

#ifdef __cplusplus
}
#endif
