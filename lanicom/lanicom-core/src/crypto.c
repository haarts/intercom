#include "lanicom/crypto.h"

#include <ctype.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "mbedtls/md.h"

#ifndef MBEDTLS_CHACHAPOLY_C
#error "lanicom needs ChaCha20-Poly1305: enable CONFIG_MBEDTLS_CHACHA20_C, _POLY1305_C and _CHACHAPOLY_C"
#endif

static const uint8_t SALT[] = "lanicom-v1";
#define SALT_LEN (sizeof(SALT) - 1)

static int hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *a, size_t a_len, const uint8_t *b,
                       size_t b_len, uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  int rc = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  if (rc == 0)
    rc = mbedtls_md_hmac_starts(&ctx, key, key_len);
  if (rc == 0 && a_len)
    rc = mbedtls_md_hmac_update(&ctx, a, a_len);
  if (rc == 0 && b_len)
    rc = mbedtls_md_hmac_update(&ctx, b, b_len);
  if (rc == 0)
    rc = mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return rc;
}

/* PBKDF2-HMAC-SHA256 with a 32-byte output (a single block). */
static int pbkdf2_sha256(const uint8_t *pw, size_t pw_len, uint32_t iterations, uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  uint8_t u[32];
  static const uint8_t block1[4] = {0, 0, 0, 1};
  mbedtls_md_init(&ctx);
  int rc = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  if (rc == 0)
    rc = mbedtls_md_hmac_starts(&ctx, pw, pw_len);
  if (rc == 0)
    rc = mbedtls_md_hmac_update(&ctx, SALT, SALT_LEN);
  if (rc == 0)
    rc = mbedtls_md_hmac_update(&ctx, block1, 4);
  if (rc == 0)
    rc = mbedtls_md_hmac_finish(&ctx, u);
  memcpy(out, u, 32);
  for (uint32_t i = 1; rc == 0 && i < iterations; i++) {
    rc = mbedtls_md_hmac_reset(&ctx);
    if (rc == 0)
      rc = mbedtls_md_hmac_update(&ctx, u, 32);
    if (rc == 0)
      rc = mbedtls_md_hmac_finish(&ctx, u);
    for (int k = 0; k < 32; k++)
      out[k] ^= u[k];
  }
  mbedtls_md_free(&ctx);
  return rc;
}

/* HKDF-Expand for L <= 32: T(1) = HMAC(prk, info || 0x01). */
static int hkdf_expand1(const uint8_t prk[32], const uint8_t *info, size_t info_len, uint8_t out[32]) {
  uint8_t buf[64];
  if (info_len + 1 > sizeof(buf))
    return -1;
  memcpy(buf, info, info_len);
  buf[info_len] = 1;
  return hmac_sha256(prk, 32, buf, info_len + 1, NULL, 0, out);
}

int lc_key_derive(lc_key_t *key, const char *key_string, uint32_t iterations) {
  const char *start = key_string;
  while (*start && isspace((unsigned char)*start))
    start++;
  size_t len = strlen(start);
  while (len && isspace((unsigned char)start[len - 1]))
    len--;
  memset(key, 0, sizeof(*key));
  if (len == 0)
    return -1;
  if (pbkdf2_sha256((const uint8_t *)start, len, iterations, key->master))
    return -1;
  if (hmac_sha256(SALT, SALT_LEN, key->master, 32, NULL, 0, key->prk))
    return -1;
  uint8_t id[32];
  if (hkdf_expand1(key->prk, (const uint8_t *)"key-id", 6, id))
    return -1;
  key->key_id = (uint16_t)(id[0] << 8 | id[1]);
  return 0;
}

int lc_send_key(const lc_key_t *key, uint32_t sender_id, uint8_t out[32]) {
  uint8_t info[10] = {'p', 'a', 'c', 'k', 'e', 't', (uint8_t)(sender_id >> 24), (uint8_t)(sender_id >> 16),
                      (uint8_t)(sender_id >> 8), (uint8_t)sender_id};
  return hkdf_expand1(key->prk, info, sizeof(info), out);
}

int lc_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                 const uint8_t *in, size_t len, uint8_t *out) {
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int rc = mbedtls_chachapoly_setkey(&ctx, key);
  if (rc == 0)
    rc = mbedtls_chachapoly_encrypt_and_tag(&ctx, len, nonce, aad, aad_len, in, out, out + len);
  mbedtls_chachapoly_free(&ctx);
  return rc;
}

int lc_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                 const uint8_t *in, size_t len, uint8_t *out) {
  if (len < 16)
    return -1;
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int rc = mbedtls_chachapoly_setkey(&ctx, key);
  if (rc == 0)
    rc = mbedtls_chachapoly_auth_decrypt(&ctx, len - 16, nonce, aad, aad_len, in + len - 16, in, out);
  mbedtls_chachapoly_free(&ctx);
  return rc;
}
