#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
extern int test_checks;

#define CHECK(cond)                                                        \
  do {                                                                     \
    test_checks++;                                                         \
    if (!(cond)) {                                                         \
      test_failures++;                                                     \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                                       \
  do {                                                                                       \
    test_checks++;                                                                           \
    long long _a = (long long)(a), _b = (long long)(b);                                      \
    if (_a != _b) {                                                                          \
      test_failures++;                                                                       \
      fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", __FILE__, __LINE__, #a, #b, _a, _b); \
    }                                                                                        \
  } while (0)

size_t hex_decode(const char *hex, unsigned char *out, size_t cap);
void hex_encode(const unsigned char *data, size_t len, char *out);

void test_vectors(void);
void test_jitter(void);
void test_engine(void);
