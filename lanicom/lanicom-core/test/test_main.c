#include "test.h"

int test_failures = 0;
int test_checks = 0;

size_t hex_decode(const char *hex, unsigned char *out, size_t cap) {
  size_t n = strlen(hex) / 2;
  if (n > cap)
    abort();
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    sscanf(hex + 2 * i, "%2x", &v);
    out[i] = (unsigned char)v;
  }
  return n;
}

void hex_encode(const unsigned char *data, size_t len, char *out) {
  for (size_t i = 0; i < len; i++)
    sprintf(out + 2 * i, "%02x", data[i]);
  out[2 * len] = 0;
}

int main(void) {
  test_vectors();
  test_jitter();
  test_engine();
  printf("%d checks, %d failures\n", test_checks, test_failures);
  return test_failures ? 1 : 0;
}
