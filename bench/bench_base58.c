#define _POSIX_C_SOURCE 200809L

#include "braid58.h"

#include <errno.h>
#include <immintrin.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef BRAID58_BENCH_TARGET
#define BRAID58_BENCH_TARGET "unknown"
#endif

enum { CORPUS_COUNT = 1024, OP_COUNT = 20, MAX_TRIALS = 99 };

char *firedancer_base58_encode_32(const unsigned char *, unsigned long *,
                                  char *);
char *firedancer_base58_encode_64(const unsigned char *, unsigned long *,
                                  char *);
unsigned char *firedancer_base58_decode_32(const char *, unsigned char *);
unsigned char *firedancer_base58_decode_64(const char *, unsigned char *);
size_t turbo_base58_encode_32(const unsigned char *, unsigned char *);
size_t turbo_base58_encode_32x3(const unsigned char *, unsigned char *,
                                unsigned char *);
size_t turbo_base58_encode_64(const unsigned char *, unsigned char *);
size_t turbo_base58_decode_32(const unsigned char *, size_t, unsigned char *);
size_t turbo_base58_decode_64(const unsigned char *, size_t, unsigned char *);
size_t five8_base58_encode_32(const unsigned char *, unsigned char *);
size_t five8_base58_encode_64(const unsigned char *, unsigned char *);
size_t five8_base58_decode_32(const unsigned char *, size_t, unsigned char *);
size_t five8_base58_decode_64(const unsigned char *, size_t, unsigned char *);

static uint8_t inputs32[CORPUS_COUNT][32] __attribute__((aligned(64)));
static uint8_t inputs64[CORPUS_COUNT][64] __attribute__((aligned(64)));
static char encoded32[CORPUS_COUNT][45] __attribute__((aligned(64)));
static char encoded64[CORPUS_COUNT][89] __attribute__((aligned(64)));
static volatile uint64_t result_sink;

static uint64_t next_random(uint64_t *state) {
  uint64_t value = *state;
  value ^= value >> 12;
  value ^= value << 25;
  value ^= value >> 27;
  *state = value;
  return value * UINT64_C(0x2545f4914f6cdd1d);
}

static uint64_t tick_start(void) {
  _mm_lfence();
  const uint64_t ticks = __rdtsc();
  _mm_lfence();
  return ticks;
}

static uint64_t tick_end(void) {
  unsigned int auxiliary;
  const uint64_t ticks = __rdtscp(&auxiliary);
  _mm_lfence();
  return ticks;
}

static double monotonic_seconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
    perror("clock_gettime");
    exit(EXIT_FAILURE);
  }
  return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static double calibrate_tsc_hz(void) {
  const double seconds_start = monotonic_seconds();
  const uint64_t ticks_start = tick_start();
  double seconds_end;
  do {
    seconds_end = monotonic_seconds();
  } while (seconds_end - seconds_start < 0.1);
  const uint64_t ticks_end = tick_end();
  return (double)(ticks_end - ticks_start) / (seconds_end - seconds_start);
}

static void fail(const char *message, size_t index) {
  fprintf(stderr, "benchmark validation failed: %s (case %zu)\n", message,
          index);
  exit(EXIT_FAILURE);
}

static void prepare_and_validate(void) {
  uint64_t state = UINT64_C(0x6a09e667f3bcc909);

  for (size_t case_index = 0; case_index < CORPUS_COUNT; ++case_index) {
    for (size_t offset = 0; offset < sizeof(inputs32[case_index]);
         offset += sizeof(uint64_t)) {
      const uint64_t word = next_random(&state);
      memcpy(inputs32[case_index] + offset, &word, sizeof(word));
    }
    for (size_t offset = 0; offset < sizeof(inputs64[case_index]);
         offset += sizeof(uint64_t)) {
      const uint64_t word = next_random(&state);
      memcpy(inputs64[case_index] + offset, &word, sizeof(word));
    }

    /* These high bits guarantee the common maximum encoded lengths. */
    inputs32[case_index][0] |= UINT8_C(0x80);
    inputs64[case_index][0] |= UINT8_C(0x80);

    unsigned long firedancer_length32 = 0;
    unsigned long firedancer_length64 = 0;
    char braid_output[45];
    char braid_output64[89];
    unsigned char turbo_output32[44];
    unsigned char turbo_output64[88];
    unsigned char five8_output32[44];
    unsigned char five8_output64[88];

    firedancer_base58_encode_32(inputs32[case_index], &firedancer_length32,
                                encoded32[case_index]);
    firedancer_base58_encode_64(inputs64[case_index], &firedancer_length64,
                                encoded64[case_index]);
    const size_t braid_length =
        braid58_encode_32(inputs32[case_index], braid_output);
    const size_t braid_length64 =
        braid58_encode_64(inputs64[case_index], braid_output64);
    const size_t turbo_length32 =
        turbo_base58_encode_32(inputs32[case_index], turbo_output32);
    const size_t turbo_length64 =
        turbo_base58_encode_64(inputs64[case_index], turbo_output64);
    const size_t five8_length32 =
        five8_base58_encode_32(inputs32[case_index], five8_output32);
    const size_t five8_length64 =
        five8_base58_encode_64(inputs64[case_index], five8_output64);

    if (firedancer_length32 != 44UL)
      fail("32-byte input did not encode to 44 characters", case_index);
    if (firedancer_length64 != 88UL)
      fail("64-byte input did not encode to 88 characters", case_index);
    if (braid_length != firedancer_length32 ||
        memcmp(braid_output, encoded32[case_index],
               firedancer_length32 + 1UL) != 0)
      fail("Braid58 and Firedancer encoders disagree", case_index);
    if (braid_length64 != firedancer_length64 ||
        memcmp(braid_output64, encoded64[case_index],
               firedancer_length64 + 1UL) != 0)
      fail("Braid58 and Firedancer 64-byte encoders disagree", case_index);
    if (turbo_length32 != firedancer_length32 ||
        memcmp(turbo_output32, encoded32[case_index], turbo_length32) != 0)
      fail("Base58 Turbo and Firedancer 32-byte encoders disagree", case_index);
    if (turbo_length64 != firedancer_length64 ||
        memcmp(turbo_output64, encoded64[case_index], turbo_length64) != 0)
      fail("Base58 Turbo and Firedancer 64-byte encoders disagree", case_index);
    if (five8_length32 != firedancer_length32 ||
        memcmp(five8_output32, encoded32[case_index], five8_length32) != 0)
      fail("five8 and Firedancer 32-byte encoders disagree", case_index);
    if (five8_length64 != firedancer_length64 ||
        memcmp(five8_output64, encoded64[case_index], five8_length64) != 0)
      fail("five8 and Firedancer 64-byte encoders disagree", case_index);

    uint8_t braid_decoded[32];
    uint8_t braid_decoded64[64];
    uint8_t firedancer_decoded32[32];
    uint8_t firedancer_decoded64[64];
    uint8_t turbo_decoded32[44];
    uint8_t turbo_decoded64[88];
    uint8_t five8_decoded32[32];
    uint8_t five8_decoded64[64];
    if (!braid58_decode_32(encoded32[case_index], firedancer_length32,
                           braid_decoded) ||
        memcmp(braid_decoded, inputs32[case_index], 32) != 0)
      fail("Braid58 decoder round trip", case_index);
    if (!braid58_decode_64(encoded64[case_index], firedancer_length64,
                           braid_decoded64) ||
        memcmp(braid_decoded64, inputs64[case_index], 64) != 0)
      fail("Braid58 64-byte decoder round trip", case_index);
    if (!firedancer_base58_decode_32(encoded32[case_index],
                                     firedancer_decoded32) ||
        memcmp(firedancer_decoded32, inputs32[case_index], 32) != 0)
      fail("Firedancer 32-byte decoder round trip", case_index);
    if (!firedancer_base58_decode_64(encoded64[case_index],
                                     firedancer_decoded64) ||
        memcmp(firedancer_decoded64, inputs64[case_index], 64) != 0)
      fail("Firedancer 64-byte decoder round trip", case_index);
    if (turbo_base58_decode_32((const unsigned char *)encoded32[case_index],
                               firedancer_length32, turbo_decoded32) != 32 ||
        memcmp(turbo_decoded32, inputs32[case_index], 32) != 0)
      fail("Base58 Turbo 32-byte decoder round trip", case_index);
    if (turbo_base58_decode_64((const unsigned char *)encoded64[case_index],
                               firedancer_length64, turbo_decoded64) != 64 ||
        memcmp(turbo_decoded64, inputs64[case_index], 64) != 0)
      fail("Base58 Turbo 64-byte decoder round trip", case_index);
    if (five8_base58_decode_32((const unsigned char *)encoded32[case_index],
                               firedancer_length32, five8_decoded32) != 32 ||
        memcmp(five8_decoded32, inputs32[case_index], 32) != 0)
      fail("five8 32-byte decoder round trip", case_index);
    if (five8_base58_decode_64((const unsigned char *)encoded64[case_index],
                               firedancer_length64, five8_decoded64) != 64 ||
        memcmp(five8_decoded64, inputs64[case_index], 64) != 0)
      fail("five8 64-byte decoder round trip", case_index);
  }

  for (size_t base = 0; base + 2U < CORPUS_COUNT; base += 3U) {
    char braid_output[3][BRAID58_ENCODED_32_CAPACITY];
    char braid_output64[3][BRAID58_ENCODED_64_CAPACITY];
    size_t braid_length[3];
    size_t braid_length64[3];
    unsigned char turbo_output[3][BRAID58_ENCODED_32_MAX_LEN];
    unsigned char turbo_output64[3][BRAID58_ENCODED_64_MAX_LEN];
    unsigned char turbo_length[3];
    size_t turbo_length64[3];
    braid58_encode_32x3(
        (const uint8_t (*)[BRAID58_BINARY_32_SIZE])&inputs32[base],
        braid_output, braid_length);
    if (turbo_base58_encode_32x3(&inputs32[base][0], &turbo_output[0][0],
                                 turbo_length) != 0)
      fail("Base58 Turbo x3 encoder failed", base);
    braid58_encode_64x3(
        (const uint8_t (*)[BRAID58_BINARY_64_SIZE])&inputs64[base],
        braid_output64, braid_length64);
    for (size_t lane = 0; lane < 3; ++lane)
      turbo_length64[lane] = turbo_base58_encode_64(
          inputs64[base + lane], turbo_output64[lane]);
    for (size_t lane = 0; lane < 3; ++lane) {
      if (braid_length[lane] != turbo_length[lane] ||
          memcmp(braid_output[lane], turbo_output[lane],
                 braid_length[lane]) != 0)
        fail("Braid58 and Base58 Turbo x3 encoders disagree", base + lane);
      if (braid_length64[lane] != turbo_length64[lane] ||
          memcmp(braid_output64[lane], turbo_output64[lane],
                 braid_length64[lane]) != 0)
        fail("Braid58 x3 and Base58 Turbo 64-byte encoders disagree",
             base + lane);
    }
  }
}

static uint64_t bench_braid_encode_32(uint64_t iterations) {
  char output[96] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length =
        braid58_encode_32(inputs32[iteration & (CORPUS_COUNT - 1)], output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)(unsigned char)output[0] + length;
  return end - start;
}

static uint64_t bench_firedancer_encode_32(uint64_t iterations) {
  char output[96] __attribute__((aligned(64)));
  unsigned long length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    firedancer_base58_encode_32(inputs32[iteration & (CORPUS_COUNT - 1)],
                                &length, output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)(unsigned char)output[0] + length;
  return end - start;
}

static uint64_t bench_turbo_encode_32(uint64_t iterations) {
  unsigned char output[64] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = turbo_base58_encode_32(inputs32[iteration & (CORPUS_COUNT - 1)],
                                    output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static uint64_t bench_braid_encode_32x3(uint64_t iterations) {
  char output[3][BRAID58_ENCODED_32_CAPACITY] __attribute__((aligned(64)));
  size_t length[3] = {0};
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const size_t base = (size_t)(iteration & UINT64_C(255)) * 3U;
    braid58_encode_32x3(
        (const uint8_t (*)[BRAID58_BINARY_32_SIZE])&inputs32[base],
        output, length);
  }
  const uint64_t end = tick_end();
  result_sink += (uint64_t)(unsigned char)output[0][0] + length[0] +
      (uint64_t)(unsigned char)output[1][0] + length[1] +
      (uint64_t)(unsigned char)output[2][0] + length[2];
  return end - start;
}

static uint64_t bench_turbo_encode_32x3(uint64_t iterations) {
  unsigned char output[3][BRAID58_ENCODED_32_MAX_LEN]
      __attribute__((aligned(64)));
  unsigned char length[3] = {0};
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const size_t base = (size_t)(iteration & UINT64_C(255)) * 3U;
    if (turbo_base58_encode_32x3(
            &inputs32[base][0], &output[0][0], length) != 0)
      exit(EXIT_FAILURE);
  }
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0][0] + length[0] +
      (uint64_t)output[1][0] + length[1] +
      (uint64_t)output[2][0] + length[2];
  return end - start;
}

static uint64_t bench_five8_encode_32(uint64_t iterations) {
  unsigned char output[64] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = five8_base58_encode_32(inputs32[iteration & (CORPUS_COUNT - 1)],
                                    output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static uint64_t bench_braid_decode_32(uint64_t iterations) {
  uint8_t output[32] __attribute__((aligned(64)));
  int success = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    success = braid58_decode_32(encoded32[iteration & (CORPUS_COUNT - 1)], 44,
                                output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + (uint64_t)success;
  return end - start;
}

static uint64_t bench_firedancer_decode_32(uint64_t iterations) {
  uint8_t output[32] __attribute__((aligned(64)));
  unsigned char *result = NULL;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    result = firedancer_base58_decode_32(
        encoded32[iteration & (CORPUS_COUNT - 1)], output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + (uint64_t)(result != NULL);
  return end - start;
}

static uint64_t bench_turbo_decode_32(uint64_t iterations) {
  uint8_t output[64] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = turbo_base58_decode_32(
        (const unsigned char *)encoded32[iteration & (CORPUS_COUNT - 1)], 44,
        output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static uint64_t bench_five8_decode_32(uint64_t iterations) {
  uint8_t output[32] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = five8_base58_decode_32(
        (const unsigned char *)encoded32[iteration & (CORPUS_COUNT - 1)], 44,
        output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static uint64_t bench_braid_encode_64(uint64_t iterations) {
  char output[96] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length =
        braid58_encode_64(inputs64[iteration & (CORPUS_COUNT - 1)], output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)(unsigned char)output[0] + length;
  return end - start;
}

static uint64_t bench_firedancer_encode_64(uint64_t iterations) {
  char output[128] __attribute__((aligned(64)));
  unsigned long length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    firedancer_base58_encode_64(inputs64[iteration & (CORPUS_COUNT - 1)],
                                &length, output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)(unsigned char)output[0] + length;
  return end - start;
}

static uint64_t bench_turbo_encode_64(uint64_t iterations) {
  unsigned char output[96] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = turbo_base58_encode_64(inputs64[iteration & (CORPUS_COUNT - 1)],
                                    output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static uint64_t bench_braid_encode_64x3(uint64_t iterations) {
  char output[3][BRAID58_ENCODED_64_CAPACITY] __attribute__((aligned(64)));
  size_t length[3] = {0};
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const size_t base = (size_t)(iteration & UINT64_C(255)) * 3U;
    braid58_encode_64x3(
        (const uint8_t (*)[BRAID58_BINARY_64_SIZE])&inputs64[base],
        output, length);
  }
  const uint64_t end = tick_end();
  result_sink += (uint64_t)(unsigned char)output[0][0] + length[0] +
      (uint64_t)(unsigned char)output[1][0] + length[1] +
      (uint64_t)(unsigned char)output[2][0] + length[2];
  return end - start;
}

static uint64_t bench_turbo_encode_64x3(uint64_t iterations) {
  unsigned char output[3][BRAID58_ENCODED_64_MAX_LEN]
      __attribute__((aligned(64)));
  size_t length[3] = {0};
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const size_t base = (size_t)(iteration & UINT64_C(255)) * 3U;
    length[0] = turbo_base58_encode_64(inputs64[base], output[0]);
    length[1] = turbo_base58_encode_64(inputs64[base + 1U], output[1]);
    length[2] = turbo_base58_encode_64(inputs64[base + 2U], output[2]);
  }
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0][0] + length[0] +
      (uint64_t)output[1][0] + length[1] +
      (uint64_t)output[2][0] + length[2];
  return end - start;
}

static uint64_t bench_five8_encode_64(uint64_t iterations) {
  unsigned char output[96] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = five8_base58_encode_64(inputs64[iteration & (CORPUS_COUNT - 1)],
                                    output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static uint64_t bench_braid_decode_64(uint64_t iterations) {
  uint8_t output[64] __attribute__((aligned(64)));
  int success = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    success = braid58_decode_64(encoded64[iteration & (CORPUS_COUNT - 1)], 88,
                                output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + (uint64_t)success;
  return end - start;
}

static uint64_t bench_firedancer_decode_64(uint64_t iterations) {
  uint8_t output[64] __attribute__((aligned(64)));
  unsigned char *result = NULL;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    result = firedancer_base58_decode_64(
        encoded64[iteration & (CORPUS_COUNT - 1)], output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + (uint64_t)(result != NULL);
  return end - start;
}

static uint64_t bench_turbo_decode_64(uint64_t iterations) {
  uint8_t output[96] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = turbo_base58_decode_64(
        (const unsigned char *)encoded64[iteration & (CORPUS_COUNT - 1)], 88,
        output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static uint64_t bench_five8_decode_64(uint64_t iterations) {
  uint8_t output[64] __attribute__((aligned(64)));
  size_t length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    length = five8_base58_decode_64(
        (const unsigned char *)encoded64[iteration & (CORPUS_COUNT - 1)], 88,
        output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + length;
  return end - start;
}

static int compare_double(const void *left, const void *right) {
  const double lhs = *(const double *)left;
  const double rhs = *(const double *)right;
  return (lhs > rhs) - (lhs < rhs);
}

static uint64_t parse_positive(const char *text, const char *name) {
  char *end = NULL;
  errno = 0;
  const unsigned long long value = strtoull(text, &end, 10);
  if (errno || !end || *end != '\0' || value == 0) {
    fprintf(stderr, "invalid %s: %s\n", name, text);
    exit(EXIT_FAILURE);
  }
  return (uint64_t)value;
}

int main(int argc, char **argv) {
  uint64_t iterations = UINT64_C(1000000);
  size_t trials = 15;
  if (argc > 1)
    iterations = parse_positive(argv[1], "iteration count");
  if (argc > 2)
    trials = (size_t)parse_positive(argv[2], "trial count");
  if (argc > 3 || trials > MAX_TRIALS) {
    fprintf(stderr, "usage: %s [iterations [trials<=%d]]\n", argv[0],
            MAX_TRIALS);
    return EXIT_FAILURE;
  }

  prepare_and_validate();
  const double tsc_hz = calibrate_tsc_hz();

  const char *const names[OP_COUNT] = {
      "Braid58 encode 32",    "Turbo encode 32",      "five8 encode 32",
      "Firedancer encode 32", "Braid58 decode 32",    "Turbo decode 32",
      "five8 decode 32",      "Firedancer decode 32", "Braid58 encode 64",
      "Turbo encode 64",      "five8 encode 64",      "Firedancer encode 64",
      "Braid58 decode 64",    "Turbo decode 64",      "five8 decode 64",
      "Firedancer decode 64", "Braid58 encode32 x3",  "Turbo encode32 x3",
      "Braid58 encode64 x3",  "Turbo encode64 x3 seq"};
  uint64_t (*const benchmarks[OP_COUNT])(uint64_t) = {
      bench_braid_encode_32, bench_turbo_encode_32,
      bench_five8_encode_32, bench_firedancer_encode_32,
      bench_braid_decode_32, bench_turbo_decode_32,
      bench_five8_decode_32, bench_firedancer_decode_32,
      bench_braid_encode_64, bench_turbo_encode_64,
      bench_five8_encode_64, bench_firedancer_encode_64,
      bench_braid_decode_64, bench_turbo_decode_64,
      bench_five8_decode_64, bench_firedancer_decode_64,
      bench_braid_encode_32x3, bench_turbo_encode_32x3,
      bench_braid_encode_64x3, bench_turbo_encode_64x3};
  const size_t bytes_per_call[OP_COUNT] = {32, 32, 32, 32, 44, 44, 44, 44,
                                           64, 64, 64, 64, 88, 88, 88, 88,
                                           96, 96, 192, 192};
  double samples[OP_COUNT][MAX_TRIALS];

  for (size_t operation = 0; operation < OP_COUNT; ++operation)
    (void)benchmarks[operation](10000);

  for (size_t trial = 0; trial < trials; ++trial) {
    for (size_t offset = 0; offset < OP_COUNT; ++offset) {
      const size_t operation = (trial + offset) % OP_COUNT;
      samples[operation][trial] =
          (double)benchmarks[operation](iterations) / (double)iterations;
    }
  }

  printf("validated: %d shared 32-byte cases and %d shared 64-byte cases\n",
         CORPUS_COUNT, CORPUS_COUNT);
  printf("Braid58 target: %s\n", BRAID58_BENCH_TARGET);
  printf("corpus: hot-cache, deterministic, 44-char/88-char encodings\n");
  printf("TSC calibration: %.3f GHz\n", tsc_hz / 1e9);
  printf("timing: unadjusted invariant-TSC ticks/call, %" PRIu64
         " calls x %zu trials\n\n",
         iterations, trials);
  printf("%-24s %10s %10s %10s %11s %10s\n", "operation", "min", "median",
         "max", "Mcalls/s", "GiB/s");
  for (size_t operation = 0; operation < OP_COUNT; ++operation) {
    double ordered[MAX_TRIALS];
    memcpy(ordered, samples[operation], trials * sizeof(double));
    qsort(ordered, trials, sizeof(double), compare_double);
    const double median_ticks = ordered[trials / 2];
    const double calls_per_second = tsc_hz / median_ticks;
    const double gib_per_second =
        calls_per_second * (double)bytes_per_call[operation] / 1073741824.0;
    printf("%-24s %10.2f %10.2f %10.2f %11.2f %10.3f\n", names[operation],
           ordered[0], ordered[trials / 2], ordered[trials - 1],
           calls_per_second / 1e6, gib_per_second);
  }

  printf("\nsink: %" PRIu64 "\n", result_sink);
  return EXIT_SUCCESS;
}
