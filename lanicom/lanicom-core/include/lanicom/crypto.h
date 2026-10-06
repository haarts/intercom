/* Key derivation and packet sealing (spec/PROTOCOL.md sections 2-3). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_PBKDF2_ITERATIONS 100000u

typedef struct {
  uint8_t master[32];
  uint8_t prk[32];
  uint16_t key_id;
} lc_key_t;

/* Derive everything from a key string (whitespace around it is ignored). 0 on success.
 * Takes about a second on an ESP32-P4: call it from a task, not the main loop. */
int lc_key_derive(lc_key_t *key, const char *key_string, uint32_t iterations);

/* send_key(sender_id) = HKDF-Expand(prk, "packet" || be32(sender_id), 32). */
int lc_send_key(const lc_key_t *key, uint32_t sender_id, uint8_t out[32]);

/* ChaCha20-Poly1305. `out` receives len + 16 bytes (ciphertext || tag). 0 on success. */
int lc_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                 const uint8_t *in, size_t len, uint8_t *out);
/* `in` is ciphertext || tag (len includes the tag). Writes len - 16 bytes. 0 if authentic. */
int lc_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                 const uint8_t *in, size_t len, uint8_t *out);

#ifdef __cplusplus
}
#endif
