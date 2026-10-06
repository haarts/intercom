#include "lanicom/packet.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void lc_header_pack(const lc_header_t *h, uint8_t out[LC_HEADER_LEN]) {
  out[0] = h->version;
  out[1] = h->type;
  out[2] = (uint8_t)(h->key_id >> 8);
  out[3] = (uint8_t)h->key_id;
  put32(out + 4, h->sender_id);
  put32(out + 8, (uint32_t)(h->epoch >> 32));
  put32(out + 12, (uint32_t)h->epoch);
  put32(out + 16, h->seq);
}

lc_err_t lc_header_parse(const uint8_t *data, size_t len, lc_header_t *h) {
  if (len < LC_MIN_PACKET || len > LC_MAX_PACKET)
    return LC_ERR_MALFORMED;
  h->version = data[0];
  h->type = data[1];
  if (h->version != LC_VERSION || (h->type != LC_TYPE_CONTROL && h->type != LC_TYPE_AUDIO))
    return LC_ERR_MALFORMED;
  h->key_id = (uint16_t)(data[2] << 8 | data[3]);
  h->sender_id = get32(data + 4);
  h->epoch = (uint64_t)get32(data + 8) << 32 | get32(data + 12);
  h->seq = get32(data + 16);
  return LC_OK;
}

int lc_seal(const uint8_t send_key[32], const lc_header_t *h, const uint8_t *plaintext, size_t len, uint8_t *out,
            size_t cap) {
  size_t total = LC_HEADER_LEN + len + LC_TAG_LEN;
  if (total > LC_MAX_PACKET || total > cap)
    return LC_ERR_TOO_LARGE;
  lc_header_pack(h, out);
  if (lc_aead_seal(send_key, out + 8, out, LC_HEADER_LEN, plaintext, len, out + LC_HEADER_LEN))
    return LC_ERR_CRYPTO;
  return (int)total;
}

int lc_open(const uint8_t send_key[32], const uint8_t *data, size_t len, uint8_t *plaintext) {
  lc_header_t h;
  if (lc_header_parse(data, len, &h) != LC_OK)
    return LC_ERR_MALFORMED;
  if (lc_aead_open(send_key, data + 8, data, LC_HEADER_LEN, data + LC_HEADER_LEN, len - LC_HEADER_LEN, plaintext))
    return LC_ERR_AUTH;
  return (int)(len - LC_MIN_PACKET);
}
