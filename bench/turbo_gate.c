#define _GNU_SOURCE 1

#include "braid58.h"

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

/* C ABI supplied by bench/turbo-public-bridge.patch.  Turbo's public decode
 * preflight requires upper-bound capacities, hence 44 and 88 writable bytes. */
size_t turbo_bench_encode_public_32(const uint8_t input[32],
                                    uint8_t output[44]);
size_t turbo_bench_decode_public_32(const uint8_t *input, size_t input_len,
                                    uint8_t output[44]);
size_t turbo_bench_encode_public_64(const uint8_t input[64],
                                    uint8_t output[88]);
size_t turbo_bench_decode_public_64(const uint8_t *input, size_t input_len,
                                    uint8_t output[88]);

enum { GROUPS = 256, REPETITIONS = 1200, TRIALS = 17 };

typedef struct {
  uint8_t input32[32];
  uint8_t input64[64];
  char text32[45];
  char text64[89];
  size_t length32;
  size_t length64;
  char candidate_encode32[45];
  char candidate_encode64[89];
  uint8_t turbo_encode32[44];
  uint8_t turbo_encode64[88];
  uint8_t candidate_decode32[32];
  uint8_t candidate_decode64[64];
  uint8_t turbo_decode32[44];
  uint8_t turbo_decode64[88];
} item;

static _Alignas(64) item corpus[GROUPS];
static volatile uint64_t sink;
static uint64_t random_state = UINT64_C(0x243f6a8885a308d3);

static uint64_t
random64(void) {
  uint64_t x = random_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  random_state = x;
  return x * UINT64_C(0x2545f4914f6cdd1d);
}

static uint64_t
tick_begin(void) {
  _mm_lfence();
  return __rdtsc();
}

static uint64_t
tick_end(void) {
  unsigned auxiliary;
  const uint64_t value = __rdtscp(&auxiliary);
  _mm_lfence();
  return value;
}

static int
compare_double(const void *left, const void *right) {
  const double a = *(const double *)left;
  const double b = *(const double *)right;
  return (a > b) - (a < b);
}

static void
fail(const char *operation, unsigned item_index, size_t got, size_t expected) {
  fprintf(stderr, "%s correctness failure at item %u: got %zu expected %zu\n",
          operation, item_index, got, expected);
  exit(2);
}

static void
fill_and_verify(int varied) {
  for (unsigned n = 0; n < GROUPS; ++n) {
    for (unsigned i = 0; i < 32; i += 8) {
      const uint64_t value = random64();
      memcpy(corpus[n].input32 + i, &value, sizeof(value));
    }
    for (unsigned i = 0; i < 64; i += 8) {
      const uint64_t value = random64();
      memcpy(corpus[n].input64 + i, &value, sizeof(value));
    }
    if (varied) {
      memset(corpus[n].input32, 0, n % 33U);
      memset(corpus[n].input64, 0, n % 65U);
    } else {
      if (corpus[n].input32[0] == 0) corpus[n].input32[0] = 1;
      if (corpus[n].input64[0] == 0) corpus[n].input64[0] = 1;
    }

    corpus[n].length32 = braid58_encode_32(
        corpus[n].input32, corpus[n].text32);
    corpus[n].length64 = braid58_encode_64(
        corpus[n].input64, corpus[n].text64);

    const size_t te32 = turbo_bench_encode_public_32(
        corpus[n].input32, corpus[n].turbo_encode32);
    const size_t te64 = turbo_bench_encode_public_64(
        corpus[n].input64, corpus[n].turbo_encode64);
    if (te32 != corpus[n].length32 ||
        memcmp(corpus[n].text32, corpus[n].turbo_encode32, te32) != 0)
      fail("Turbo encode32", n, te32, corpus[n].length32);
    if (te64 != corpus[n].length64 ||
        memcmp(corpus[n].text64, corpus[n].turbo_encode64, te64) != 0)
      fail("Turbo encode64", n, te64, corpus[n].length64);

    if (!braid58_decode_32(
            corpus[n].text32, corpus[n].length32,
            corpus[n].candidate_decode32) ||
        memcmp(corpus[n].candidate_decode32, corpus[n].input32, 32) != 0)
      fail("Braid decode32", n, 0, 1);
    if (!braid58_decode_64(
            corpus[n].text64, corpus[n].length64,
            corpus[n].candidate_decode64) ||
        memcmp(corpus[n].candidate_decode64, corpus[n].input64, 64) != 0)
      fail("Braid decode64", n, 0, 1);

    const size_t td32 = turbo_bench_decode_public_32(
        (const uint8_t *)corpus[n].text32, corpus[n].length32,
        corpus[n].turbo_decode32);
    const size_t td64 = turbo_bench_decode_public_64(
        (const uint8_t *)corpus[n].text64, corpus[n].length64,
        corpus[n].turbo_decode64);
    if (td32 != 32 ||
        memcmp(corpus[n].turbo_decode32, corpus[n].input32, 32) != 0)
      fail("Turbo decode32", n, td32, 32);
    if (td64 != 64 ||
        memcmp(corpus[n].turbo_decode64, corpus[n].input64, 64) != 0)
      fail("Turbo decode64", n, td64, 64);
  }
}

#define MEASURE_ENCODE(NAME, FUNCTION, INPUT, OUTPUT)                        \
  static double NAME(void) {                                                \
    uint64_t checksum = 0;                                                  \
    const uint64_t begin = tick_begin();                                    \
    for (unsigned r = 0; r < REPETITIONS; ++r) {                            \
      for (unsigned n = 0; n < GROUPS; ++n) {                               \
        const size_t length = FUNCTION(corpus[n].INPUT, corpus[n].OUTPUT);   \
        checksum += corpus[n].OUTPUT[(r + n) % length];                      \
      }                                                                      \
    }                                                                        \
    const uint64_t end = tick_end();                                        \
    sink += checksum;                                                       \
    return (double)(end - begin) / (double)(GROUPS * REPETITIONS);          \
  }

#define MEASURE_DECODE(NAME, FUNCTION, TEXT, LENGTH, OUTPUT, WIDTH)           \
  static double NAME(void) {                                                \
    uint64_t checksum = 0;                                                  \
    const uint64_t begin = tick_begin();                                    \
    for (unsigned r = 0; r < REPETITIONS; ++r) {                            \
      for (unsigned n = 0; n < GROUPS; ++n) {                               \
        const size_t result = (size_t)FUNCTION(                             \
            (const void *)corpus[n].TEXT, corpus[n].LENGTH,                 \
            corpus[n].OUTPUT);                                              \
        checksum += result + corpus[n].OUTPUT[(r + n) % (WIDTH)];           \
      }                                                                      \
    }                                                                        \
    const uint64_t end = tick_end();                                        \
    sink += checksum;                                                       \
    return (double)(end - begin) / (double)(GROUPS * REPETITIONS);          \
  }

MEASURE_ENCODE(measure_braid_encode32, braid58_encode_32,
               input32, candidate_encode32)
MEASURE_ENCODE(measure_turbo_encode32, turbo_bench_encode_public_32,
               input32, turbo_encode32)
MEASURE_ENCODE(measure_braid_encode64, braid58_encode_64,
               input64, candidate_encode64)
MEASURE_ENCODE(measure_turbo_encode64, turbo_bench_encode_public_64,
               input64, turbo_encode64)

/* Braid's success result is 1 while Turbo returns decoded length.  Both are
 * consumed only to keep the call live; output bytes are the compared work. */
MEASURE_DECODE(measure_braid_decode32, braid58_decode_32,
               text32, length32, candidate_decode32, 32)
MEASURE_DECODE(measure_turbo_decode32, turbo_bench_decode_public_32,
               text32, length32, turbo_decode32, 32)
MEASURE_DECODE(measure_braid_decode64, braid58_decode_64,
               text64, length64, candidate_decode64, 64)
MEASURE_DECODE(measure_turbo_decode64, turbo_bench_decode_public_64,
               text64, length64, turbo_decode64, 64)

typedef double (*measure_function)(void);

static int
compare(const char *label, measure_function braid, measure_function turbo) {
  double b[TRIALS], t[TRIALS], paired[TRIALS];
  (void)braid();
  (void)turbo();
  for (unsigned trial = 0; trial < TRIALS; ++trial) {
    if (trial & 1U) {
      t[trial] = turbo();
      b[trial] = braid();
    } else {
      b[trial] = braid();
      t[trial] = turbo();
    }
    paired[trial] = 100.0 * (b[trial] / t[trial] - 1.0);
  }
  qsort(b, TRIALS, sizeof(*b), compare_double);
  qsort(t, TRIALS, sizeof(*t), compare_double);
  qsort(paired, TRIALS, sizeof(*paired), compare_double);
  const int pass = b[TRIALS / 2] < t[TRIALS / 2] && paired[TRIALS / 2] < 0.0;
  printf("%-18s Braid %8.3f  Turbo %8.3f ticks  paired %+7.3f%%  %s\n",
         label, b[TRIALS / 2], t[TRIALS / 2], paired[TRIALS / 2],
         pass ? "PASS" : "FAIL");
  return pass;
}

static unsigned
run_corpus(int varied) {
  unsigned failures = 0;
  fill_and_verify(varied);
  puts(varied ? "corpus: leading-zero rotation" : "corpus: full width");
  failures += (unsigned)!compare(
      "encode32", measure_braid_encode32, measure_turbo_encode32);
  failures += (unsigned)!compare(
      "decode32", measure_braid_decode32, measure_turbo_decode32);
  failures += (unsigned)!compare(
      "encode64", measure_braid_encode64, measure_turbo_encode64);
  failures += (unsigned)!compare(
      "decode64", measure_braid_decode64, measure_turbo_decode64);
  return failures;
}

int
main(void) {
  if (!__builtin_cpu_supports("avx2") ||
      !__builtin_cpu_supports("avx512f") ||
      !__builtin_cpu_supports("avx512bw") ||
      !__builtin_cpu_supports("avx512vbmi") ||
      !__builtin_cpu_supports("avx512ifma")) {
    fputs("required AVX2/AVX-512 features are unavailable\n", stderr);
    return 77;
  }
  const int cpu = sched_getcpu();
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
  }
  unsigned failures = run_corpus(0);
  failures += run_corpus(1);
  printf("gate=%s failures=%u sink=%llu cpu=%d\n",
         failures == 0 ? "PASS" : "FAIL", failures,
         (unsigned long long)sink, cpu);
  return failures == 0 ? 0 : 1;
}
