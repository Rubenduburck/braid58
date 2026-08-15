#include "braid58.h"

#include <errno.h>
#include <immintrin.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CORPUS_COUNT = 1024,
  OP_COUNT = 6,
  MAX_TRIALS = 99
};

char *firedancer_base58_encode_32(const unsigned char *, unsigned long *,
                                  char *);
char *firedancer_base58_encode_64(const unsigned char *, unsigned long *,
                                  char *);
unsigned char *firedancer_base58_decode_32(const char *, unsigned char *);
unsigned char *firedancer_base58_decode_64(const char *, unsigned char *);

static uint8_t inputs32[CORPUS_COUNT][32] __attribute__((aligned(64)));
static uint8_t inputs64[CORPUS_COUNT][64] __attribute__((aligned(64)));
static char encoded32[CORPUS_COUNT][45] __attribute__((aligned(64)));
static char encoded64[CORPUS_COUNT][89] __attribute__((aligned(64)));
static volatile uint64_t result_sink;

static uint64_t
next_random(uint64_t *state) {
  uint64_t value = *state;
  value ^= value >> 12;
  value ^= value << 25;
  value ^= value >> 27;
  *state = value;
  return value * UINT64_C(0x2545f4914f6cdd1d);
}

static uint64_t
tick_start(void) {
  _mm_lfence();
  const uint64_t ticks = __rdtsc();
  _mm_lfence();
  return ticks;
}

static uint64_t
tick_end(void) {
  unsigned int auxiliary;
  const uint64_t ticks = __rdtscp(&auxiliary);
  _mm_lfence();
  return ticks;
}

static void
fail(const char *message, size_t index) {
  fprintf(stderr, "benchmark validation failed: %s (case %zu)\n",
          message, index);
  exit(EXIT_FAILURE);
}

static void
prepare_and_validate(void) {
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
    unsigned long braid_length = 0;

    firedancer_base58_encode_32(inputs32[case_index],
                                &firedancer_length32,
                                encoded32[case_index]);
    firedancer_base58_encode_64(inputs64[case_index],
                                &firedancer_length64,
                                encoded64[case_index]);
    fd_base58_encode_32(inputs32[case_index], &braid_length, braid_output);

    if (firedancer_length32 != 44UL)
      fail("32-byte input did not encode to 44 characters", case_index);
    if (firedancer_length64 != 88UL)
      fail("64-byte input did not encode to 88 characters", case_index);
    if (braid_length != firedancer_length32 ||
        memcmp(braid_output, encoded32[case_index],
               firedancer_length32 + 1UL) != 0)
      fail("Braid58 and Firedancer encoders disagree", case_index);

    uint8_t braid_decoded[32];
    uint8_t firedancer_decoded32[32];
    uint8_t firedancer_decoded64[64];
    if (!braid58_decode_32(encoded32[case_index], firedancer_length32,
                           braid_decoded) ||
        memcmp(braid_decoded, inputs32[case_index], 32) != 0)
      fail("Braid58 decoder round trip", case_index);
    if (!firedancer_base58_decode_32(encoded32[case_index],
                                     firedancer_decoded32) ||
        memcmp(firedancer_decoded32, inputs32[case_index], 32) != 0)
      fail("Firedancer 32-byte decoder round trip", case_index);
    if (!firedancer_base58_decode_64(encoded64[case_index],
                                     firedancer_decoded64) ||
        memcmp(firedancer_decoded64, inputs64[case_index], 64) != 0)
      fail("Firedancer 64-byte decoder round trip", case_index);
  }
}

static uint64_t
bench_braid_encode_32(uint64_t iterations) {
  char output[96] __attribute__((aligned(64)));
  unsigned long length = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    fd_base58_encode_32(inputs32[iteration & (CORPUS_COUNT - 1)],
                        &length, output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)(unsigned char)output[0] + length;
  return end - start;
}

static uint64_t
bench_firedancer_encode_32(uint64_t iterations) {
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

static uint64_t
bench_braid_decode_32(uint64_t iterations) {
  uint8_t output[32] __attribute__((aligned(64)));
  int success = 0;
  const uint64_t start = tick_start();
  for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    success = braid58_decode_32(
        encoded32[iteration & (CORPUS_COUNT - 1)], 44, output);
  const uint64_t end = tick_end();
  result_sink += (uint64_t)output[0] + (uint64_t)success;
  return end - start;
}

static uint64_t
bench_firedancer_decode_32(uint64_t iterations) {
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

static uint64_t
bench_firedancer_encode_64(uint64_t iterations) {
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

static uint64_t
bench_firedancer_decode_64(uint64_t iterations) {
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

static int
compare_double(const void *left, const void *right) {
  const double lhs = *(const double *)left;
  const double rhs = *(const double *)right;
  return (lhs > rhs) - (lhs < rhs);
}

static uint64_t
parse_positive(const char *text, const char *name) {
  char *end = NULL;
  errno = 0;
  const unsigned long long value = strtoull(text, &end, 10);
  if (errno || !end || *end != '\0' || value == 0) {
    fprintf(stderr, "invalid %s: %s\n", name, text);
    exit(EXIT_FAILURE);
  }
  return (uint64_t)value;
}

int
main(int argc, char **argv) {
  uint64_t iterations = UINT64_C(1000000);
  size_t trials = 15;
  if (argc > 1) iterations = parse_positive(argv[1], "iteration count");
  if (argc > 2) trials = (size_t)parse_positive(argv[2], "trial count");
  if (argc > 3 || trials > MAX_TRIALS) {
    fprintf(stderr, "usage: %s [iterations [trials<=%d]]\n",
            argv[0], MAX_TRIALS);
    return EXIT_FAILURE;
  }

  prepare_and_validate();

  const char *const names[OP_COUNT] = {
      "Braid58 encode 32", "Firedancer encode 32",
      "Braid58 decode 32", "Firedancer decode 32",
      "Firedancer encode 64", "Firedancer decode 64"};
  uint64_t (*const benchmarks[OP_COUNT])(uint64_t) = {
      bench_braid_encode_32, bench_firedancer_encode_32,
      bench_braid_decode_32, bench_firedancer_decode_32,
      bench_firedancer_encode_64, bench_firedancer_decode_64};
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

  printf("validated: %d shared 32-byte cases and %d Firedancer 64-byte cases\n",
         CORPUS_COUNT, CORPUS_COUNT);
  printf("corpus: hot-cache, deterministic, 44-char/88-char encodings\n");
  printf("timing: unadjusted invariant-TSC ticks/call, %" PRIu64
         " calls x %zu trials\n\n",
         iterations, trials);
  printf("%-24s %10s %10s %10s\n", "operation", "min", "median", "max");
  for (size_t operation = 0; operation < OP_COUNT; ++operation) {
    double ordered[MAX_TRIALS];
    memcpy(ordered, samples[operation], trials * sizeof(double));
    qsort(ordered, trials, sizeof(double), compare_double);
    printf("%-24s %10.2f %10.2f %10.2f\n",
           names[operation], ordered[0], ordered[trials / 2],
           ordered[trials - 1]);
  }

  printf("\nsink: %" PRIu64 "\n", result_sink);
  return EXIT_SUCCESS;
}
