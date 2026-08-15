/*
 * Fixed-32-byte Bitcoin Base58 encoder for AVX2-only CPUs.
 *
 * Ten radix-2^26 chunks are converted into nine radix-58^5 cells. Since
 * 58^5 fits below 2^30, every matrix term is an exact AVX2 vpmuludq instead
 * of an emulated packed 64-bit product.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define BRAID58_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define BRAID58_TARGET_AVX2
#endif

#define BRAID58_B5 UINT64_C(656356768)
#define BRAID58_B3 UINT32_C(195112)
#define BRAID58_B2 UINT32_C(3364)
#define BRAID58_CHUNK26_MASK UINT64_C(0x3ffffff)

/* Little-endian radix-58^5 expansion of 2^(26*i). */
static const uint64_t BRAID58_W5_ENCODE[10][8] __attribute__((aligned(32))) = {
    {UINT64_C(1), 0, 0, 0, 0, 0, 0, 0},
    {UINT64_C(67108864), 0, 0, 0, 0, 0, 0, 0},
    {UINT64_C(443814048), UINT64_C(6861511), 0, 0, 0, 0, 0, 0},
    {UINT64_C(437973984), UINT64_C(506963877), UINT64_C(701551), 0, 0, 0, 0, 0},
    {UINT64_C(192414240), UINT64_C(584944171), UINT64_C(527870455),
     UINT64_C(71729), 0, 0, 0, 0},
    {UINT64_C(85356032), UINT64_C(237320048), UINT64_C(230466711),
     UINT64_C(641497958), UINT64_C(7333), 0, 0, 0},
    {UINT64_C(59086336), UINT64_C(425716400), UINT64_C(469620603),
     UINT64_C(369434287), UINT64_C(563670112), UINT64_C(749), 0, 0},
    {UINT64_C(249488768), UINT64_C(563819044), UINT64_C(543108756),
     UINT64_C(589143927), UINT64_C(28959436), UINT64_C(439056932), UINT64_C(76),
     0},
    {UINT64_C(147129216), UINT64_C(329522580), UINT64_C(449746271),
     UINT64_C(218521078), UINT64_C(590921969), UINT64_C(502289454),
     UINT64_C(550667440), UINT64_C(7)},
    {UINT64_C(576074656), UINT64_C(432829647), UINT64_C(553198108),
     UINT64_C(378465102), UINT64_C(185459504), UINT64_C(598754100),
     UINT64_C(313589673), UINT64_C(526064760)},
};

/* Expand a four-bit scalar carry mask into four byte values 0/1. */
static const uint32_t BRAID58_EXPAND4[16] __attribute__((aligned(64))) = {
    0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001,
    0x00010100, 0x00010101, 0x01000000, 0x01000001, 0x01000100, 0x01000101,
    0x01010000, 0x01010001, 0x01010100, 0x01010101,
};

/* Four 16-entry alphabet quarters, duplicated in both AVX2 half-lanes. */
static const uint8_t BRAID58_MAP0[] __attribute__((aligned(32))) =
    "123456789ABCDEFG123456789ABCDEFG";
static const uint8_t BRAID58_MAP1[] __attribute__((aligned(32))) =
    "HJKLMNPQRSTUVWXYHJKLMNPQRSTUVWXY";
static const uint8_t BRAID58_MAP2[] __attribute__((aligned(32))) =
    "ZabcdefghijkmnopZabcdefghijkmnop";
static const uint8_t BRAID58_MAP3[] __attribute__((aligned(32))) =
    "qrstuvwxyz\0\0\0\0\0\0qrstuvwxyz\0\0\0\0\0\0";

/* Shuffle networks that assemble four five-digit cell records per lane. */
static const uint8_t BRAID58_FIELDS_A[32] __attribute__((aligned(32))) = {
    4, 5, 6, 7, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80, 0, 1, 2, 3,
    4, 5, 6, 7, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80, 0, 1, 2, 3,
};
static const uint8_t BRAID58_FIELDS_B[32] __attribute__((aligned(32))) = {
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 4,    5,    6,
    7,    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 4,    5,    6,    7,    0x80, 0x80, 0x80, 0x80,
};
static const uint8_t BRAID58_HEAD_A[32] __attribute__((aligned(32))) = {
    0x80, 15, 11, 7, 3, 0x80, 14, 10, 6, 2, 0x80, 13, 9, 5, 1, 0x80,
    0x80, 15, 11, 7, 3, 0x80, 14, 10, 6, 2, 0x80, 13, 9, 5, 1, 0x80,
};
static const uint8_t BRAID58_HEAD_B[32] __attribute__((aligned(32))) = {
    3,    0x80, 0x80, 0x80, 0x80, 2,    0x80, 0x80, 0x80, 0x80, 1,
    0x80, 0x80, 0x80, 0x80, 0,    3,    0x80, 0x80, 0x80, 0x80, 2,
    0x80, 0x80, 0x80, 0x80, 1,    0x80, 0x80, 0x80, 0x80, 0,
};
static const uint8_t BRAID58_TAIL_A[32] __attribute__((aligned(32))) = {
    12,   8,    4,    0,    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 12,   8,    4,    0,    0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

static inline uint64_t braid58_load_be64(const void *source) {
  uint64_t value;
  memcpy(&value, source, sizeof(value));
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap64(value);
#else
  return ((value & UINT64_C(0x00000000000000ff)) << 56) |
         ((value & UINT64_C(0x000000000000ff00)) << 40) |
         ((value & UINT64_C(0x0000000000ff0000)) << 24) |
         ((value & UINT64_C(0x00000000ff000000)) << 8) |
         ((value & UINT64_C(0x000000ff00000000)) >> 8) |
         ((value & UINT64_C(0x0000ff0000000000)) >> 24) |
         ((value & UINT64_C(0x00ff000000000000)) >> 40) |
         ((value & UINT64_C(0xff00000000000000)) >> 56);
#endif
}

/* Exact floor(x / 58^5) and remainder for every raw encoder column. */
BRAID58_TARGET_AVX2 static inline void
braid58_div_b5_4(__m256i value, __m256i *quotient, __m256i *remainder) {
  const __m256i magic = _mm256_set1_epi64x((long long)UINT64_C(3513093979));
  const __m256i divisor = _mm256_set1_epi64x((long long)BRAID58_B5);
  const __m256i product0 = _mm256_mul_epu32(value, magic);
  const __m256i product1 =
      _mm256_mul_epu32(_mm256_srli_epi64(value, 32), magic);
  __m256i q = _mm256_srli_epi64(
      _mm256_add_epi64(product1, _mm256_srli_epi64(product0, 32)), 29);
  __m256i product = _mm256_mul_epu32(q, divisor);
  const __m256i high = _mm256_cmpgt_epi64(product, value);
  q = _mm256_add_epi64(q, high);
  product = _mm256_sub_epi64(product, _mm256_and_si256(high, divisor));
  *quotient = q;
  *remainder = _mm256_sub_epi64(value, product);
}

BRAID58_TARGET_AVX2 static inline __m256i braid58_div195112_u32(__m256i value) {
  const __m256i magic = _mm256_set1_epi32(721316415);
  const __m256i even = _mm256_mul_epu32(value, magic);
  const __m256i odd = _mm256_mul_epu32(_mm256_srli_epi64(value, 32), magic);
  const __m256i high =
      _mm256_blend_epi32(_mm256_shuffle_epi32(even, 0xf5), odd, 0xaa);
  return _mm256_srli_epi32(high, 15);
}

BRAID58_TARGET_AVX2 static inline __m256i braid58_map58(__m256i digit) {
  const __m256i index = _mm256_and_si256(digit, _mm256_set1_epi8(15));
  const __m256i group_bit4 = _mm256_slli_epi16(digit, 3);
  const __m256i group_bit5 = _mm256_slli_epi16(digit, 2);
  const __m256i low_half = _mm256_blendv_epi8(
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_MAP0),
          index),
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_MAP1),
          index),
      group_bit4);
  const __m256i high_half = _mm256_blendv_epi8(
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_MAP2),
          index),
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_MAP3),
          index),
      group_bit4);
  return _mm256_blendv_epi8(low_half, high_half, group_bit5);
}

/* Convert one 256-bit value to cells 0..7 and return cell 8 separately. */
BRAID58_TARGET_AVX2 static inline __m256i
braid58_encode_cells(const uint8_t input[32], uint32_t *top_cell) {
  const uint64_t word0 = braid58_load_be64(input + 24);
  const uint64_t word1 = braid58_load_be64(input + 16);
  const uint64_t word2 = braid58_load_be64(input + 8);
  const uint64_t word3 = braid58_load_be64(input);
  const uint64_t a2 = ((word0 >> 52) | (word1 << 12)) & BRAID58_CHUNK26_MASK;
  const uint64_t a3 = (word1 >> 14) & BRAID58_CHUNK26_MASK;
  const uint64_t a4 = ((word1 >> 40) | (word2 << 24)) & BRAID58_CHUNK26_MASK;
  const uint64_t a5 = (word2 >> 2) & BRAID58_CHUNK26_MASK;
  const uint64_t a6 = (word2 >> 28) & BRAID58_CHUNK26_MASK;
  const uint64_t a7 = ((word2 >> 54) | (word3 << 10)) & BRAID58_CHUNK26_MASK;
  const uint64_t a8 = (word3 >> 16) & BRAID58_CHUNK26_MASK;
  const uint64_t a9 = word3 >> 42;

  __m256i column_lo = _mm256_setr_epi64x(
      (long long)(word0 & ((UINT64_C(1) << 52) - 1)), 0, 0, 0);
  __m256i column_hi = _mm256_setzero_si256();
#define BRAID58_ADD_LO(row, value)                                             \
  column_lo = _mm256_add_epi64(                                                \
      column_lo,                                                               \
      _mm256_mul_epu32(                                                        \
          _mm256_set1_epi64x((long long)(value)),                              \
          _mm256_load_si256(                                                   \
              (const __m256i *)(const void *)&BRAID58_W5_ENCODE[(row)][0])))
#define BRAID58_ADD_HI(row, value)                                             \
  column_hi = _mm256_add_epi64(                                                \
      column_hi,                                                               \
      _mm256_mul_epu32(                                                        \
          _mm256_set1_epi64x((long long)(value)),                              \
          _mm256_load_si256(                                                   \
              (const __m256i *)(const void *)&BRAID58_W5_ENCODE[(row)][4])))
  BRAID58_ADD_LO(2, a2);
  BRAID58_ADD_LO(3, a3);
  BRAID58_ADD_LO(4, a4);
  BRAID58_ADD_LO(5, a5);
  BRAID58_ADD_HI(5, a5);
  BRAID58_ADD_LO(6, a6);
  BRAID58_ADD_HI(6, a6);
  BRAID58_ADD_LO(7, a7);
  BRAID58_ADD_HI(7, a7);
  BRAID58_ADD_LO(8, a8);
  BRAID58_ADD_HI(8, a8);
  BRAID58_ADD_LO(9, a9);
  BRAID58_ADD_HI(9, a9);
#undef BRAID58_ADD_LO
#undef BRAID58_ADD_HI

  __m256i qlo, qhi, rlo, rhi;
  braid58_div_b5_4(column_lo, &qlo, &rlo);
  braid58_div_b5_4(column_hi, &qhi, &rhi);

  const __m256i zero = _mm256_setzero_si256();
  __m256i qprev_lo = _mm256_permute4x64_epi64(qlo, 0x90);
  qprev_lo = _mm256_blend_epi32(zero, qprev_lo, 0xfc);
  const __m256i cross = _mm256_permute2x128_si256(qlo, qhi, 0x21);
  const __m256i qprev_hi = _mm256_alignr_epi8(qhi, cross, 8);
  __m256i sum_lo = _mm256_add_epi64(rlo, qprev_lo);
  __m256i sum_hi = _mm256_add_epi64(rhi, qprev_hi);

  const __m256i propagate = _mm256_set1_epi64x((long long)(BRAID58_B5 - 1));
  unsigned generate = (unsigned)_mm256_movemask_pd(
      _mm256_castsi256_pd(_mm256_cmpgt_epi64(sum_lo, propagate)));
  generate |= (unsigned)_mm256_movemask_pd(
                  _mm256_castsi256_pd(_mm256_cmpgt_epi64(sum_hi, propagate)))
              << 4;
  unsigned propagate_mask = (unsigned)_mm256_movemask_pd(
      _mm256_castsi256_pd(_mm256_cmpeq_epi64(sum_lo, propagate)));
  propagate_mask |= (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(
                        _mm256_cmpeq_epi64(sum_hi, propagate)))
                    << 4;
  generate |= propagate_mask & (generate << 1);
  propagate_mask &= propagate_mask << 1;
  generate |= propagate_mask & (generate << 2);
  propagate_mask &= propagate_mask << 2;
  generate |= propagate_mask & (generate << 4);
  generate &= 0xffU;

  const unsigned carry_in = (generate << 1) & 0xffU;
  const __m256i carry_lo = _mm256_cvtepu8_epi64(
      _mm_cvtsi32_si128((int)BRAID58_EXPAND4[carry_in & 15U]));
  const __m256i carry_hi = _mm256_cvtepu8_epi64(
      _mm_cvtsi32_si128((int)BRAID58_EXPAND4[carry_in >> 4]));
  sum_lo = _mm256_add_epi64(sum_lo, carry_lo);
  sum_hi = _mm256_add_epi64(sum_hi, carry_hi);
  const __m256i divisor = _mm256_set1_epi64x((long long)BRAID58_B5);
  const __m256i generate_lo = _mm256_cmpgt_epi64(sum_lo, propagate);
  const __m256i generate_hi = _mm256_cmpgt_epi64(sum_hi, propagate);
  sum_lo = _mm256_sub_epi64(sum_lo, _mm256_and_si256(generate_lo, divisor));
  sum_hi = _mm256_sub_epi64(sum_hi, _mm256_and_si256(generate_hi, divisor));

  *top_cell =
      (uint32_t)(uint64_t)_mm256_extract_epi64(qhi, 3) + ((generate >> 7) & 1U);
  const __m256i index = _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0);
  const __m256i low_dwords = _mm256_permutevar8x32_epi32(sum_lo, index);
  const __m256i high_dwords = _mm256_permutevar8x32_epi32(sum_hi, index);
  return _mm256_permute2x128_si256(low_dwords, high_dwords, 0x20);
}

BRAID58_TARGET_AVX2 size_t
braid58_encode_32_avx2(const uint8_t input[static 32],
                       char output[static BRAID58_ENCODED_32_CAPACITY]) {
  uint8_t digits[64] __attribute__((aligned(32)));
  uint8_t ascii[64] __attribute__((aligned(32)));
  uint32_t top_cell;
  const __m256i zero = _mm256_setzero_si256();
  const __m256i cell = braid58_encode_cells(input, &top_cell);

  const __m256i top_pair = braid58_div195112_u32(cell);
  const __m256i bottom3 = _mm256_sub_epi32(
      cell, _mm256_mullo_epi32(top_pair, _mm256_set1_epi32((int)BRAID58_B3)));
  const __m256i quarter = _mm256_srli_epi32(bottom3, 2);
  const __m256i pair = _mm256_packus_epi32(top_pair, quarter);
  const __m256i first_magic =
      _mm256_setr_epi16(1130, 1130, 1130, 1130, (short)39899, (short)39899,
                        (short)39899, (short)39899, 1130, 1130, 1130, 1130,
                        (short)39899, (short)39899, (short)39899, (short)39899);
  const __m256i first_high = _mm256_mulhi_epu16(pair, first_magic);
  const __m256i high =
      _mm256_blend_epi16(first_high, _mm256_srli_epi16(first_high, 9), 0xf0);
  const __m256i first_coefficient = _mm256_setr_epi16(
      58, 58, 58, 58, 841, 841, 841, 841, 58, 58, 58, 58, 841, 841, 841, 841);
  const __m256i first_remainder =
      _mm256_sub_epi16(pair, _mm256_mullo_epi16(high, first_coefficient));
  const __m256i scale =
      _mm256_setr_epi16(1, 1, 1, 1, 4, 4, 4, 4, 1, 1, 1, 1, 4, 4, 4, 4);
  __m256i pair58 = _mm256_mullo_epi16(first_remainder, scale);
  const __m256i low2 = _mm256_and_si256(bottom3, _mm256_set1_epi32(3));
  pair58 = _mm256_add_epi16(pair58, _mm256_packus_epi32(zero, low2));
  const __m256i middle = _mm256_mulhi_epu16(pair58, _mm256_set1_epi16(1130));
  const __m256i low = _mm256_sub_epi16(
      pair58, _mm256_mullo_epi16(middle, _mm256_set1_epi16(58)));

  const __m256i byte_fields = _mm256_packus_epi16(low, middle);
  const __m256i top_field = _mm256_packus_epi16(high, zero);
  const __m256i fields = _mm256_or_si256(
      _mm256_shuffle_epi8(
          byte_fields,
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_FIELDS_A)),
      _mm256_shuffle_epi8(
          top_field,
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_FIELDS_B)));
  const __m256i head = _mm256_or_si256(
      _mm256_shuffle_epi8(
          fields,
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_HEAD_A)),
      _mm256_shuffle_epi8(
          top_field,
          _mm256_load_si256((const __m256i *)(const void *)BRAID58_HEAD_B)));
  const __m256i tail = _mm256_shuffle_epi8(
      fields, _mm256_load_si256((const __m256i *)(const void *)BRAID58_TAIL_A));

  _mm256_store_si256((__m256i *)(void *)digits, zero);
  _mm256_store_si256((__m256i *)(void *)(digits + 32), zero);
  const __m128i head_lo = _mm256_castsi256_si128(head);
  const __m128i head_hi = _mm256_extracti128_si256(head, 1);
  const __m128i tail_lo = _mm256_castsi256_si128(tail);
  const __m128i tail_hi = _mm256_extracti128_si256(tail, 1);
  _mm_storeu_si128((__m128i *)(void *)(digits + 5), head_hi);
  const uint32_t tail_hi_word = (uint32_t)_mm_cvtsi128_si32(tail_hi);
  memcpy(digits + 21, &tail_hi_word, sizeof(tail_hi_word));
  _mm_storeu_si128((__m128i *)(void *)(digits + 25), head_lo);
  const uint32_t tail_lo_word = (uint32_t)_mm_cvtsi128_si32(tail_lo);
  memcpy(digits + 41, &tail_lo_word, sizeof(tail_lo_word));

  const uint32_t top2 = top_cell / BRAID58_B2;
  const uint32_t top_remainder = top_cell - top2 * BRAID58_B2;
  const uint32_t top0 = top2 / 58U;
  const uint32_t top1 = top2 - top0 * 58U;
  const uint32_t top3 = top_remainder / 58U;
  const uint32_t top4 = top_remainder - top3 * 58U;
  digits[0] = 0;
  digits[1] = (uint8_t)top0;
  digits[2] = (uint8_t)top1;
  digits[3] = (uint8_t)top3;
  digits[4] = (uint8_t)top4;

  const __m256i digit0 =
      _mm256_load_si256((const __m256i *)(const void *)digits);
  const __m256i digit1 =
      _mm256_load_si256((const __m256i *)(const void *)(digits + 32));
  uint64_t nonzero_digits = (uint32_t)~(uint32_t)_mm256_movemask_epi8(
      _mm256_cmpeq_epi8(digit0, zero));
  nonzero_digits |= (uint64_t)(uint32_t)~(uint32_t)_mm256_movemask_epi8(
                        _mm256_cmpeq_epi8(digit1, zero))
                    << 32;
  nonzero_digits &= (UINT64_C(1) << 45) - 1;
  const unsigned first_digit =
      nonzero_digits ? (unsigned)__builtin_ctzll(nonzero_digits) : 45U;

  _mm256_store_si256((__m256i *)(void *)ascii, braid58_map58(digit0));
  _mm256_store_si256((__m256i *)(void *)(ascii + 32), braid58_map58(digit1));

  const __m256i input_vector =
      _mm256_loadu_si256((const __m256i *)(const void *)input);
  const uint32_t nonzero_bytes =
      ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(input_vector, zero));
  const unsigned leading_zero_bytes =
      nonzero_bytes ? (unsigned)__builtin_ctz(nonzero_bytes) : 32U;

  const unsigned skip = first_digit - leading_zero_bytes;
  const size_t length = 45U - skip;
  _mm256_storeu_si256(
      (__m256i *)(void *)output,
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii + skip)));
  _mm_storeu_si128(
      (__m128i *)(void *)(output + length - 16U),
      _mm_loadu_si128((const __m128i *)(const void *)(ascii + 29)));
  output[length] = '\0';
  return length;
}
