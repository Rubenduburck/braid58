#include "braid58.h"
#include "braid58_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static void
fail(const char *message, uint64_t iteration) {
  fprintf(stderr, "test failure: %s at iteration %llu\n", message,
          (unsigned long long)iteration);
  exit(EXIT_FAILURE);
}

static uint64_t
next_random(uint64_t *state) {
  uint64_t value = *state;
  value ^= value >> 12;
  value ^= value << 25;
  value ^= value >> 27;
  *state = value;
  return value * UINT64_C(0x2545f4914f6cdd1d);
}

static void
check_value(const uint8_t input[32], uint64_t iteration) {
  char expected[45];
  char actual[45];
  uint8_t decoded[32];
  const size_t expected_len = braid58_encode_32_scalar(input, expected);
  const size_t actual_len = braid58_encode_32(input, actual);

  if (actual_len != expected_len ||
      memcmp(actual, expected, expected_len + 1U) != 0)
    fail("public encoder differential", iteration);
  if (!braid58_decode_32(actual, actual_len, decoded) ||
      memcmp(decoded, input, sizeof(decoded)) != 0)
    fail("public round trip", iteration);

#if BRAID58_HAVE_AVX2_KERNEL
  if (braid58_get_backend() != BRAID58_BACKEND_SCALAR) {
    char avx[45];
    const size_t avx_len = braid58_encode_32_avx2(input, avx);
    if (avx_len != expected_len ||
        memcmp(avx, expected, expected_len + 1U) != 0)
      fail("AVX2 encoder differential", iteration);
    if (!braid58_decode_32_avx2(avx, avx_len, decoded) ||
        memcmp(decoded, input, sizeof(decoded)) != 0)
      fail("AVX2 round trip", iteration);
  }
#endif

#if BRAID58_HAVE_AVX512_KERNEL
  if (braid58_get_backend() == BRAID58_BACKEND_AVX512) {
    char avx[45];
    const size_t avx_len = braid58_encode_32_avx512(input, avx);
    if (avx_len != expected_len ||
        memcmp(avx, expected, expected_len + 1U) != 0)
      fail("AVX-512 encoder differential", iteration);
    if (!braid58_decode_32_avx512(avx, avx_len, decoded) ||
        memcmp(decoded, input, sizeof(decoded)) != 0)
      fail("AVX-512 round trip", iteration);
  }
#endif
}

static void
check_api(void) {
  static const uint8_t input[32] = {
      24, 243, 6, 223, 230, 153, 210, 8, 92, 137, 123, 67, 164, 197, 79, 196,
      125, 43, 183, 85, 103, 91, 232, 167, 73, 131, 104, 131, 0, 101, 214, 231};
  static const char expected[] =
      "2gPihUTjt3FJqf1VpidgrY5cZ6PuyMccGVwQHRfjMPZG";
  char encoded[45];
  uint8_t decoded[32];

  const size_t length = braid58_encode_32(input, encoded);
  if (length != sizeof(expected) - 1U || strcmp(encoded, expected) != 0)
    fail("known encoding", 0);
  if (!braid58_decode_32(expected, sizeof(expected) - 1U, decoded) ||
      memcmp(decoded, input, sizeof(decoded)) != 0)
    fail("known decoding", 0);
  if (braid58_encode_32(NULL, encoded) != 0 ||
      braid58_encode_32(input, NULL) != 0)
    fail("NULL encoder argument", 0);
  if (braid58_decode_32(NULL, sizeof(expected) - 1U, decoded) != 0 ||
      braid58_decode_32(expected, sizeof(expected) - 1U, NULL) != 0)
    fail("NULL decoder argument", 0);

  memset(decoded, 0xa5, sizeof(decoded));
  if (braid58_decode_32("invalid", 7, decoded) != 0)
    fail("invalid input accepted", 0);
  for (size_t i = 0; i < sizeof(decoded); ++i)
    if (decoded[i] != UINT8_C(0xa5))
      fail("failure changed output", i);

  const braid58_backend backend = braid58_get_backend();
  if (backend != BRAID58_BACKEND_SCALAR &&
      backend != BRAID58_BACKEND_AVX2 &&
      backend != BRAID58_BACKEND_AVX512)
    fail("unknown backend", (uint64_t)backend);
}

static void
check_invalid_inputs(void) {
  uint8_t input[32];
  uint8_t output[32];
  char encoded[45];
  memset(input, 0xa5, sizeof(input));
  const size_t length = braid58_encode_32_scalar(input, encoded);

  for (unsigned byte = 0; byte < 256; ++byte) {
    if (memchr(ALPHABET, byte, 58) != NULL)
      continue;
    char damaged[45];
    memcpy(damaged, encoded, length);
    damaged[length / 2U] = (char)(uint8_t)byte;
    if (braid58_decode_32(damaged, length, output))
      fail("invalid alphabet byte accepted", byte);
  }

  memset(encoded, 'z', 44);
  if (braid58_decode_32(encoded, 44, output))
    fail("overflow accepted", 0);
  memset(encoded, '1', 32);
  encoded[32] = '2';
  if (braid58_decode_32(encoded, 33, output))
    fail("noncanonical leading one accepted", 0);
  if (braid58_decode_32(encoded, 31, output) ||
      braid58_decode_32(encoded, 45, output))
    fail("invalid length accepted", 0);
}

static void
check_arbitrary_strings(uint64_t *state) {
  char encoded[44];
  uint8_t expected[32];
  uint8_t actual[32];

  for (uint64_t iteration = 0; iteration < UINT64_C(200000); ++iteration) {
    const size_t length = 32U + (size_t)(next_random(state) % 13U);
    for (size_t i = 0; i < length; ++i)
      encoded[i] = ALPHABET[next_random(state) % 58U];

    const int expected_ok =
        braid58_decode_32_scalar(encoded, length, expected);
    const int actual_ok = braid58_decode_32(encoded, length, actual);
    if (actual_ok != expected_ok ||
        (actual_ok && memcmp(actual, expected, sizeof(actual)) != 0))
      fail("decoder differential", iteration);

#if BRAID58_HAVE_AVX2_KERNEL
    if (braid58_get_backend() != BRAID58_BACKEND_SCALAR) {
      const int avx_ok = braid58_decode_32_avx2(encoded, length, actual);
      if (avx_ok != expected_ok ||
          (avx_ok && memcmp(actual, expected, sizeof(actual)) != 0))
        fail("AVX2 decoder differential", iteration);
    }
#endif

#if BRAID58_HAVE_AVX512_KERNEL
    if (braid58_get_backend() == BRAID58_BACKEND_AVX512) {
      const int avx_ok = braid58_decode_32_avx512(encoded, length, actual);
      if (avx_ok != expected_ok ||
          (avx_ok && memcmp(actual, expected, sizeof(actual)) != 0))
        fail("AVX-512 decoder differential", iteration);
    }
#endif
  }
}

int
main(void) {
  uint64_t state = UINT64_C(0x6a09e667f3bcc909);
  uint8_t input[32] = {0};

  check_api();
  check_value(input, 0);
  input[31] = 1;
  check_value(input, 1);
  memset(input, 0xff, sizeof(input));
  check_value(input, 2);

  for (unsigned zeroes = 0; zeroes <= 32; ++zeroes) {
    memset(input, 0, sizeof(input));
    for (unsigned i = zeroes; i < 32; i += 8) {
      const uint64_t value = next_random(&state);
      const size_t count = 32U - i < 8U ? 32U - i : 8U;
      memcpy(input + i, &value, count);
    }
    if (zeroes < 32 && input[zeroes] == 0)
      input[zeroes] = 1;
    check_value(input, zeroes);
  }

  for (uint64_t iteration = 0; iteration < UINT64_C(2000000); ++iteration) {
    for (unsigned i = 0; i < 32; i += 8) {
      const uint64_t value = next_random(&state);
      memcpy(input + i, &value, sizeof(value));
    }
    if (iteration < UINT64_C(1000000) && input[0] == 0)
      input[0] = 1;
    check_value(input, iteration);
  }

  check_arbitrary_strings(&state);
  check_invalid_inputs();
  printf("braid58 C tests passed (backend: %s)\n",
         braid58_get_backend() == BRAID58_BACKEND_AVX512 ? "AVX-512" :
         braid58_get_backend() == BRAID58_BACKEND_AVX2 ? "AVX2" : "scalar");
  return EXIT_SUCCESS;
}
