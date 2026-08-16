#include "braid58.h"
#include "braid58_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static void fail(const char *message, uint64_t iteration) {
  fprintf(stderr, "test failure: %s at iteration %llu\n", message,
          (unsigned long long)iteration);
  exit(EXIT_FAILURE);
}

static uint64_t next_random(uint64_t *state) {
  uint64_t value = *state;
  value ^= value >> 12;
  value ^= value << 25;
  value ^= value >> 27;
  *state = value;
  return value * UINT64_C(0x2545f4914f6cdd1d);
}

static void check_value(const uint8_t input[32], uint64_t iteration) {
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
  char avx2[45];
  const size_t avx2_len = braid58_encode_32_avx2(input, avx2);
  if (avx2_len != expected_len ||
      memcmp(avx2, expected, expected_len + 1U) != 0)
    fail("AVX2 encoder differential", iteration);
  if (!braid58_decode_32_avx2(avx2, avx2_len, decoded) ||
      memcmp(decoded, input, sizeof(decoded)) != 0)
    fail("AVX2 round trip", iteration);
#endif

#if BRAID58_HAVE_AVX512_KERNEL
  char avx512[45];
  const size_t avx512_len = braid58_encode_32_avx512(input, avx512);
  if (avx512_len != expected_len ||
      memcmp(avx512, expected, expected_len + 1U) != 0)
    fail("AVX-512 encoder differential", iteration);
  if (!braid58_decode_32_avx512(avx512, avx512_len, decoded) ||
      memcmp(decoded, input, sizeof(decoded)) != 0)
    fail("AVX-512 round trip", iteration);
#endif
}

static void check_value_64(const uint8_t input[64], uint64_t iteration) {
  char expected[89];
  char actual[89];
  uint8_t decoded[64];
  const size_t expected_len = braid58_encode_64_scalar(input, expected);
  const size_t actual_len = braid58_encode_64(input, actual);

  if (actual_len != expected_len ||
      memcmp(actual, expected, expected_len + 1U) != 0)
    fail("public 64-byte encoder differential", iteration);
  if (!braid58_decode_64(actual, actual_len, decoded) ||
      memcmp(decoded, input, sizeof(decoded)) != 0)
    fail("public 64-byte round trip", iteration);

#if BRAID58_HAVE_AVX2_KERNEL
  char avx2[89];
  const size_t avx2_len = braid58_encode_64_avx2(input, avx2);
  if (avx2_len != expected_len ||
      memcmp(avx2, expected, expected_len + 1U) != 0)
    fail("AVX2 64-byte encoder differential", iteration);
  if (!braid58_decode_64_avx2(avx2, avx2_len, decoded) ||
      memcmp(decoded, input, sizeof(decoded)) != 0)
    fail("AVX2 64-byte round trip", iteration);
#endif

#if BRAID58_HAVE_AVX512_KERNEL
  char avx512[89];
  const size_t avx512_len = braid58_encode_64_avx512(input, avx512);
  if (avx512_len != expected_len ||
      memcmp(avx512, expected, expected_len + 1U) != 0)
    fail("AVX-512 64-byte encoder differential", iteration);
  if (!braid58_decode_64_avx512(avx512, avx512_len, decoded) ||
      memcmp(decoded, input, sizeof(decoded)) != 0)
    fail("AVX-512 64-byte round trip", iteration);
#endif
}

static void check_api(void) {
  static const uint8_t input[32] = {24,  243, 6,   223, 230, 153, 210, 8,
                                    92,  137, 123, 67,  164, 197, 79,  196,
                                    125, 43,  183, 85,  103, 91,  232, 167,
                                    73,  131, 104, 131, 0,   101, 214, 231};
  static const char expected[] = "2gPihUTjt3FJqf1VpidgrY5cZ6PuyMccGVwQHRfjMPZG";
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

  uint8_t input64[64];
  uint8_t decoded64[64];
  char expected64[89];
  char encoded64[89];
  for (unsigned i = 0; i < 64; ++i)
    input64[i] = (uint8_t)(3U * i + 1U);
  const size_t expected64_len = braid58_encode_64_scalar(input64, expected64);
  const size_t length64 = braid58_encode_64(input64, encoded64);
  if (length64 != expected64_len ||
      memcmp(encoded64, expected64, length64 + 1U) != 0)
    fail("known 64-byte encoding", 0);
  if (!braid58_decode_64(encoded64, length64, decoded64) ||
      memcmp(decoded64, input64, sizeof(decoded64)) != 0)
    fail("known 64-byte decoding", 0);
  if (braid58_encode_64(NULL, encoded64) != 0 ||
      braid58_encode_64(input64, NULL) != 0 ||
      braid58_decode_64(NULL, length64, decoded64) != 0 ||
      braid58_decode_64(encoded64, length64, NULL) != 0)
    fail("NULL 64-byte argument", 0);

  memset(decoded, 0xa5, sizeof(decoded));
  if (braid58_decode_32("invalid", 7, decoded) != 0)
    fail("invalid input accepted", 0);
  for (size_t i = 0; i < sizeof(decoded); ++i)
    if (decoded[i] != UINT8_C(0xa5))
      fail("failure changed output", i);

}

static void check_invalid_inputs(void) {
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

  uint8_t input64[64];
  uint8_t output64[64];
  char encoded64[89];
  memset(input64, 0xa5, sizeof(input64));
  const size_t length64 = braid58_encode_64_scalar(input64, encoded64);
  for (unsigned byte = 0; byte < 256; ++byte) {
    if (memchr(ALPHABET, byte, 58) != NULL)
      continue;
    const char saved = encoded64[length64 / 2U];
    encoded64[length64 / 2U] = (char)(uint8_t)byte;
    memset(output64, 0xa5, sizeof(output64));
    if (braid58_decode_64(encoded64, length64, output64))
      fail("invalid 64-byte alphabet byte accepted", byte);
    for (size_t i = 0; i < sizeof(output64); ++i)
      if (output64[i] != UINT8_C(0xa5))
        fail("failed 64-byte decode changed output", byte);
    encoded64[length64 / 2U] = saved;
  }
  memset(encoded64, 'z', 88);
  if (braid58_decode_64(encoded64, 88, output64))
    fail("64-byte overflow accepted", 0);
  memset(encoded64, '1', 64);
  encoded64[64] = '2';
  if (braid58_decode_64(encoded64, 65, output64))
    fail("noncanonical 64-byte leading one accepted", 0);
  if (braid58_decode_64(encoded64, 63, output64) ||
      braid58_decode_64(encoded64, 89, output64))
    fail("invalid 64-byte length accepted", 0);
}

static void check_arbitrary_strings(uint64_t *state) {
  char encoded[44];
  uint8_t expected[32];
  uint8_t actual[32];

  for (uint64_t iteration = 0; iteration < UINT64_C(200000); ++iteration) {
    const size_t length = 32U + (size_t)(next_random(state) % 13U);
    for (size_t i = 0; i < length; ++i)
      encoded[i] = ALPHABET[next_random(state) % 58U];

    const int expected_ok = braid58_decode_32_scalar(encoded, length, expected);
    const int actual_ok = braid58_decode_32(encoded, length, actual);
    if (actual_ok != expected_ok ||
        (actual_ok && memcmp(actual, expected, sizeof(actual)) != 0))
      fail("decoder differential", iteration);

#if BRAID58_HAVE_AVX2_KERNEL
    const int avx2_ok = braid58_decode_32_avx2(encoded, length, actual);
    if (avx2_ok != expected_ok ||
        (avx2_ok && memcmp(actual, expected, sizeof(actual)) != 0))
      fail("AVX2 decoder differential", iteration);
#endif

#if BRAID58_HAVE_AVX512_KERNEL
    const int avx512_ok = braid58_decode_32_avx512(encoded, length, actual);
    if (avx512_ok != expected_ok ||
        (avx512_ok && memcmp(actual, expected, sizeof(actual)) != 0))
      fail("AVX-512 decoder differential", iteration);
#endif
  }
}

static void check_arbitrary_strings_64(uint64_t *state) {
  char encoded[88];
  uint8_t expected[64];
  uint8_t actual[64];

  for (uint64_t iteration = 0; iteration < UINT64_C(100000); ++iteration) {
    const size_t length = 64U + (size_t)(next_random(state) % 25U);
    for (size_t i = 0; i < length; ++i)
      encoded[i] = ALPHABET[next_random(state) % 58U];

    const int expected_ok = braid58_decode_64_scalar(encoded, length, expected);
    const int actual_ok = braid58_decode_64(encoded, length, actual);
    if (actual_ok != expected_ok ||
        (actual_ok && memcmp(actual, expected, sizeof(actual)) != 0))
      fail("64-byte decoder differential", iteration);

#if BRAID58_HAVE_AVX2_KERNEL
    const int avx2_ok = braid58_decode_64_avx2(encoded, length, actual);
    if (avx2_ok != expected_ok ||
        (avx2_ok && memcmp(actual, expected, sizeof(actual)) != 0))
      fail("AVX2 64-byte decoder differential", iteration);
#endif

#if BRAID58_HAVE_AVX512_KERNEL
    const int avx512_ok = braid58_decode_64_avx512(encoded, length, actual);
    if (avx512_ok != expected_ok ||
        (avx512_ok && memcmp(actual, expected, sizeof(actual)) != 0))
      fail("AVX-512 64-byte decoder differential", iteration);
#endif
  }
}

static void check_batch_encoders(uint64_t *state) {
  uint8_t input32[3][32];
  uint8_t input64[3][64];
  char output32[3][45];
  char output64[3][89];
  size_t length32[3];
  size_t length64[3];

  for (uint64_t iteration = 0; iteration < UINT64_C(200000); ++iteration) {
    for (unsigned lane = 0; lane < 3; ++lane) {
      for (unsigned offset = 0; offset < 32; offset += 8) {
        const uint64_t value = next_random(state);
        memcpy(input32[lane] + offset, &value, sizeof(value));
      }
      for (unsigned offset = 0; offset < 64; offset += 8) {
        const uint64_t value = next_random(state);
        memcpy(input64[lane] + offset, &value, sizeof(value));
      }
      const size_t zeroes32 = (size_t)((iteration + lane) % 33U);
      const size_t zeroes64 = (size_t)((iteration + lane) % 65U);
      memset(input32[lane], 0, zeroes32);
      memset(input64[lane], 0, zeroes64);
      if (zeroes32 < 32 && input32[lane][zeroes32] == 0)
        input32[lane][zeroes32] = 1;
      if (zeroes64 < 64 && input64[lane][zeroes64] == 0)
        input64[lane][zeroes64] = 1;
    }

    braid58_encode_32x3(input32, output32, length32);
    braid58_encode_64x3(input64, output64, length64);
    for (unsigned lane = 0; lane < 3; ++lane) {
      char expected32[45];
      char expected64[89];
      const size_t expected_len32 =
          braid58_encode_32_scalar(input32[lane], expected32);
      const size_t expected_len64 =
          braid58_encode_64_scalar(input64[lane], expected64);
      if (length32[lane] != expected_len32 ||
          memcmp(output32[lane], expected32, expected_len32 + 1U) != 0)
        fail("32-byte x3 encoder differential", iteration * 3U + lane);
      if (length64[lane] != expected_len64 ||
          memcmp(output64[lane], expected64, expected_len64 + 1U) != 0)
        fail("64-byte x3 encoder differential", iteration * 3U + lane);
    }

    size_t pair_len32[2];
    size_t pair_len64[2];
    char pair32[2][45];
    char pair64[2][89];
    braid58_encode_32x2(input32, pair32, pair_len32);
    braid58_encode_64x2(input64, pair64, pair_len64);
    for (unsigned lane = 0; lane < 2; ++lane) {
      if (pair_len32[lane] != length32[lane] ||
          memcmp(pair32[lane], output32[lane], length32[lane] + 1U) != 0)
        fail("32-byte x2/x3 disagreement", iteration * 2U + lane);
      if (pair_len64[lane] != length64[lane] ||
          memcmp(pair64[lane], output64[lane], length64[lane] + 1U) != 0)
        fail("64-byte x2/x3 disagreement", iteration * 2U + lane);
    }
  }
}

int main(void) {
  uint64_t state = UINT64_C(0x6a09e667f3bcc909);
  uint8_t input[32] = {0};
  uint8_t input64[64] = {0};

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

  check_value_64(input64, 0);
  input64[63] = 1;
  check_value_64(input64, 1);
  memset(input64, 0xff, sizeof(input64));
  check_value_64(input64, 2);
  for (unsigned zeroes = 0; zeroes <= 64; ++zeroes) {
    memset(input64, 0, sizeof(input64));
    for (unsigned i = zeroes; i < 64; i += 8) {
      const uint64_t value = next_random(&state);
      const size_t count = 64U - i < 8U ? 64U - i : 8U;
      memcpy(input64 + i, &value, count);
    }
    if (zeroes < 64 && input64[zeroes] == 0)
      input64[zeroes] = 1;
    check_value_64(input64, zeroes);
  }
  for (uint64_t iteration = 0; iteration < UINT64_C(500000); ++iteration) {
    for (unsigned i = 0; i < 64; i += 8) {
      const uint64_t value = next_random(&state);
      memcpy(input64 + i, &value, sizeof(value));
    }
    if (iteration < UINT64_C(250000) && input64[0] == 0)
      input64[0] = 1;
    check_value_64(input64, iteration);
  }

  check_arbitrary_strings(&state);
  check_arbitrary_strings_64(&state);
  check_batch_encoders(&state);
  check_invalid_inputs();
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512
  puts("braid58 C tests passed (target: AVX-512)");
#elif BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  puts("braid58 C tests passed (target: AVX2)");
#else
  puts("braid58 C tests passed (target: scalar)");
#endif
  return EXIT_SUCCESS;
}
