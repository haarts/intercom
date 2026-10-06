/* Interop helper for the Python tests:
 *   lanicom_tool seal <key> <type> <sender_hex> <epoch_hex> <seq> <plaintext_hex>  -> packet hex
 *   lanicom_tool open <key> <packet_hex>  -> "<type> <sender_hex> <epoch_hex> <seq> <plaintext_hex>" or "error <reason>"
 *   lanicom_tool control <hex>            -> re-encoded hex (decode + encode round trip) or "error"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lanicom/lanicom.h"

static size_t unhex(const char *hex, uint8_t *out, size_t cap) {
  size_t n = strlen(hex) / 2;
  if (n > cap)
    exit(2);
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(hex + 2 * i, "%2x", &v) != 1)
      exit(2);
    out[i] = (uint8_t)v;
  }
  return n;
}

static void put_hex(const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++)
    printf("%02x", d[i]);
}

int main(int argc, char **argv) {
  static uint8_t a[4096], b[4096];
  lc_key_t key;
  if (argc == 8 && strcmp(argv[1], "seal") == 0) {
    if (lc_key_derive(&key, argv[2], LC_PBKDF2_ITERATIONS))
      return 1;
    lc_header_t h = {LC_VERSION, (uint8_t)atoi(argv[3]), key.key_id, (uint32_t)strtoul(argv[4], NULL, 16),
                     strtoull(argv[5], NULL, 16), (uint32_t)strtoul(argv[6], NULL, 10)};
    size_t n = unhex(argv[7], a, sizeof(a));
    uint8_t sk[32];
    lc_send_key(&key, h.sender_id, sk);
    int len = lc_seal(sk, &h, a, n, b, sizeof(b));
    if (len < 0)
      return 1;
    put_hex(b, (size_t)len);
    printf("\n");
    return 0;
  }
  if (argc == 4 && strcmp(argv[1], "open") == 0) {
    if (lc_key_derive(&key, argv[2], LC_PBKDF2_ITERATIONS))
      return 1;
    size_t n = unhex(argv[3], a, sizeof(a));
    lc_header_t h;
    if (lc_header_parse(a, n, &h) != LC_OK) {
      printf("error malformed\n");
      return 0;
    }
    if (h.key_id != key.key_id) {
      printf("error foreign\n");
      return 0;
    }
    uint8_t sk[32];
    lc_send_key(&key, h.sender_id, sk);
    int len = lc_open(sk, a, n, b);
    if (len < 0) {
      printf("error auth\n");
      return 0;
    }
    printf("%u %08x %016llx %u ", h.type, h.sender_id, (unsigned long long)h.epoch, h.seq);
    put_hex(b, (size_t)len);
    printf("\n");
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "control") == 0) {
    size_t n = unhex(argv[2], a, sizeof(a));
    lc_control_t msg;
    if (lc_control_decode(a, n, &msg) || msg.type == LC_MSG_NONE) {
      printf("error\n");
      return 0;
    }
    int len = lc_control_encode(&msg, b, sizeof(b));
    put_hex(b, (size_t)(len < 0 ? 0 : len));
    printf("\n");
    return 0;
  }
  fprintf(stderr, "usage: see the comment at the top of lanicom_tool.c\n");
  return 2;
}
