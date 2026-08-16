/*
 * AVX2 Bitcoin Base58 encoder for 32-byte inputs.
 *
 * AVX2 has four 64-bit lanes per YMM and no packed 64-bit multiply. The
 * matrix therefore uses radix 58^5:
 *
 *   encoder: ten base-2^26 chunks -> nine base-58^5 cells.
 *
 * Since 58^5 is below 2^30, every matrix coefficient fits in 32 bits and
 * VPMULUDQ produces complete 64-bit products. Raw columns remain below 2^58.
 *
 * ISA: Haswell-class x86-64. Backend selection occurs at compile time.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define B58_B5 UINT64_C(656356768)
#define B58_B3 UINT32_C(195112)
#define B58_B2 UINT32_C(3364)
#define B58_CHUNK26_MASK UINT64_C(0x3ffffff)

/*
 * Row i is the little-endian radix-58^5 expansion of 2^(26*i).
 * Row zero and row one are folded into the scalar low-52-bit seed.
 */
_Alignas(32) static const uint64_t B58_W5_ENCODE[10][8] = {
    {UINT64_C(1), 0, 0, 0, 0, 0, 0, 0},
    {UINT64_C(67108864), 0, 0, 0, 0, 0, 0, 0},
    {UINT64_C(443814048), UINT64_C(6861511), 0, 0, 0, 0, 0, 0},
    {UINT64_C(437973984), UINT64_C(506963877), UINT64_C(701551), 0,
     0, 0, 0, 0},
    {UINT64_C(192414240), UINT64_C(584944171), UINT64_C(527870455),
     UINT64_C(71729), 0, 0, 0, 0},
    {UINT64_C(85356032), UINT64_C(237320048), UINT64_C(230466711),
     UINT64_C(641497958), UINT64_C(7333), 0, 0, 0},
    {UINT64_C(59086336), UINT64_C(425716400), UINT64_C(469620603),
     UINT64_C(369434287), UINT64_C(563670112), UINT64_C(749), 0, 0},
    {UINT64_C(249488768), UINT64_C(563819044), UINT64_C(543108756),
     UINT64_C(589143927), UINT64_C(28959436), UINT64_C(439056932),
     UINT64_C(76), 0},
    {UINT64_C(147129216), UINT64_C(329522580), UINT64_C(449746271),
     UINT64_C(218521078), UINT64_C(590921969), UINT64_C(502289454),
     UINT64_C(550667440), UINT64_C(7)},
    {UINT64_C(576074656), UINT64_C(432829647), UINT64_C(553198108),
     UINT64_C(378465102), UINT64_C(185459504), UINT64_C(598754100),
     UINT64_C(313589673), UINT64_C(526064760)},
};

/* Expand a four-bit scalar carry mask into four byte values 0/1. */
_Alignas(64) static const uint32_t B58_EXPAND4[16] = {
    0x00000000,0x00000001,0x00000100,0x00000101,
    0x00010000,0x00010001,0x00010100,0x00010101,
    0x01000000,0x01000001,0x01000100,0x01000101,
    0x01010000,0x01010001,0x01010100,0x01010101,
};

/* Four 16-entry alphabet quarters, duplicated for both AVX2 half-lanes. */
_Alignas(32) static const uint8_t B58_MAP0[] =
    "123456789ABCDEFG123456789ABCDEFG";
_Alignas(32) static const uint8_t B58_MAP1[] =
    "HJKLMNPQRSTUVWXYHJKLMNPQRSTUVWXY";
_Alignas(32) static const uint8_t B58_MAP2[] =
    "ZabcdefghijkmnopZabcdefghijkmnop";
_Alignas(32) static const uint8_t B58_MAP3[] =
    "qrstuvwxyz\0\0\0\0\0\0qrstuvwxyz\0\0\0\0\0\0";

/* Cell-field assembly masks.  The split produces fields in four-cell
 * half-lanes; these shuffles emit cells 7..0 in five-digit order. */
_Alignas(32) static const uint8_t B58_FIELDS_A[32] = {
     4, 5, 6, 7,12,13,14,15, 0x80,0x80,0x80,0x80, 0,1,2,3,
     4, 5, 6, 7,12,13,14,15, 0x80,0x80,0x80,0x80, 0,1,2,3,
};
_Alignas(32) static const uint8_t B58_FIELDS_B[32] = {
    0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 4,5,6,7,
    0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    4,5,6,7, 0x80,0x80,0x80,0x80,
};
_Alignas(32) static const uint8_t B58_HEAD_A[32] = {
    0x80,15,11,7,3, 0x80,14,10,6,2, 0x80,13,9,5,1, 0x80,
    0x80,15,11,7,3, 0x80,14,10,6,2, 0x80,13,9,5,1, 0x80,
};
_Alignas(32) static const uint8_t B58_HEAD_B[32] = {
    3,0x80,0x80,0x80,0x80, 2,0x80,0x80,0x80,0x80,
    1,0x80,0x80,0x80,0x80, 0,
    3,0x80,0x80,0x80,0x80, 2,0x80,0x80,0x80,0x80,
    1,0x80,0x80,0x80,0x80, 0,
};
_Alignas(32) static const uint8_t B58_TAIL_A[32] = {
    12,8,4,0, 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x80,0x80,0x80,0x80,
    12,8,4,0, 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x80,0x80,0x80,0x80,
};

static inline uint64_t
b58_load_be64(const void *src) {
  uint64_t value;
  memcpy(&value, src, sizeof(value));
  return __builtin_bswap64(value);
}

/* Exact floor(x/58) for every uint32_t x.  Spell out the reciprocal so
 * target-specific compilers do not choose a scalar IDIV when both quotient
 * and remainder are consumed by the top-cell splitter. */
static inline uint32_t
b58_div58_u32(uint32_t x) {
  return (uint32_t)(((uint64_t)x * UINT64_C(0x8d3dcb09)) >> 37);
}

/* Exact floor(x/B58_B5), remainder, for every encoder raw-column bound.
 * M=ceil(2^61/B).  Since x<2^58, the estimate has error below 0.063 and is
 * either exact or one high. */
static inline void
b58_div_b5_8(__m256i xlo, __m256i xhi,
             __m256i *qlo_out, __m256i *qhi_out,
             __m256i *rlo_out, __m256i *rhi_out) {
  const __m256i magic = _mm256_set1_epi64x((long long)UINT64_C(3513093979));
  const __m256i divisor = _mm256_set1_epi64x((long long)B58_B5);
  const __m256i lo0 = _mm256_mul_epu32(xlo, magic);
  const __m256i lo1 = _mm256_mul_epu32(_mm256_srli_epi64(xlo, 32), magic);
  const __m256i hi0 = _mm256_mul_epu32(xhi, magic);
  const __m256i hi1 = _mm256_mul_epu32(_mm256_srli_epi64(xhi, 32), magic);
  __m256i qlo = _mm256_srli_epi64(
      _mm256_add_epi64(lo1, _mm256_srli_epi64(lo0, 32)), 29);
  __m256i qhi = _mm256_srli_epi64(
      _mm256_add_epi64(hi1, _mm256_srli_epi64(hi0, 32)), 29);
  __m256i plo = _mm256_mul_epu32(qlo, divisor);
  __m256i phi = _mm256_mul_epu32(qhi, divisor);
  const __m256i hlo = _mm256_cmpgt_epi64(plo, xlo);
  const __m256i hhi = _mm256_cmpgt_epi64(phi, xhi);
  const __m256i any = _mm256_or_si256(hlo, hhi);
  if (__builtin_expect(!_mm256_testz_si256(any, any), 0)) {
    qlo = _mm256_add_epi64(qlo, hlo); /* masks are -1 in corrected lanes */
    qhi = _mm256_add_epi64(qhi, hhi);
    plo = _mm256_sub_epi64(plo, _mm256_and_si256(hlo, divisor));
    phi = _mm256_sub_epi64(phi, _mm256_and_si256(hhi, divisor));
  }
  *qlo_out = qlo;
  *qhi_out = qhi;
  *rlo_out = _mm256_sub_epi64(xlo, plo);
  *rhi_out = _mm256_sub_epi64(xhi, phi);
}

static inline __m256i
b58_div195112_u32(__m256i value) {
  const __m256i magic = _mm256_set1_epi32(721316415);
  const __m256i even = _mm256_mul_epu32(value, magic);
  const __m256i odd =
      _mm256_mul_epu32(_mm256_srli_epi64(value, 32), magic);
  const __m256i high = _mm256_blend_epi32(
      _mm256_shuffle_epi32(even, 0xf5), odd, 0xaa);
  return _mm256_srli_epi32(high, 15);
}

typedef struct {
  uint64_t packed;
  uint32_t first;
} b58_top_digits;

static inline b58_top_digits
b58_split_top_simd(uint32_t top_cell) {
  const uint32_t top2 = top_cell / B58_B2;
  const uint32_t rem2 = top_cell - top2 * B58_B2;
  const __m128i pair = _mm_cvtsi32_si128((int)(top2 | (rem2 << 16)));
  const __m128i quotient = _mm_mulhi_epu16(pair, _mm_set1_epi16(1130));
  const __m128i remainder = _mm_sub_epi16(
      pair, _mm_mullo_epi16(quotient, _mm_set1_epi16(58)));
  const __m128i interleaved = _mm_unpacklo_epi16(quotient, remainder);
  const __m128i bytes = _mm_packus_epi16(interleaved, _mm_setzero_si128());
  const b58_top_digits result = {
      (uint64_t)(uint32_t)_mm_cvtsi128_si32(bytes) << 8,
      (uint32_t)_mm_cvtsi128_si32(quotient) & 0xffffU};
  return result;
}

static inline __m256i
b58_map58(__m256i digit) {
  const __m256i index =
      _mm256_and_si256(digit, _mm256_set1_epi8(15));
  const __m256i group_bit4 = _mm256_slli_epi16(digit, 3);
  const __m256i group_bit5 = _mm256_slli_epi16(digit, 2);
  const __m256i low_half = _mm256_blendv_epi8(
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)B58_MAP0), index),
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)B58_MAP1), index),
      group_bit4);
  const __m256i high_half = _mm256_blendv_epi8(
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)B58_MAP2), index),
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)(const void *)B58_MAP3), index),
      group_bit4);
  return _mm256_blendv_epi8(low_half, high_half, group_bit5);
}

typedef struct {
  __m256i lo;
  __m256i hi;
} b58_raw_columns;

/* Branch-free input rechunk and radix-matrix stage. */
static inline b58_raw_columns
b58_make_raw_columns(const uint8_t input[32]) {
  const uint64_t word0 = b58_load_be64(input + 24);
  const uint64_t word1 = b58_load_be64(input + 16);
  const uint64_t word2 = b58_load_be64(input + 8);
  const uint64_t word3 = b58_load_be64(input + 0);
  const uint64_t a2 =
      ((word0 >> 52) | (word1 << 12)) & B58_CHUNK26_MASK;
  const uint64_t a3 = (word1 >> 14) & B58_CHUNK26_MASK;
  const uint64_t a4 =
      ((word1 >> 40) | (word2 << 24)) & B58_CHUNK26_MASK;
  const uint64_t a5 = (word2 >> 2) & B58_CHUNK26_MASK;
  const uint64_t a6 = (word2 >> 28) & B58_CHUNK26_MASK;
  const uint64_t a7 =
      ((word2 >> 54) | (word3 << 10)) & B58_CHUNK26_MASK;
  const uint64_t a8 = (word3 >> 16) & B58_CHUNK26_MASK;
  const uint64_t a9 = word3 >> 42;

  __m256i column_lo = _mm256_setr_epi64x(
      (long long)(word0 & ((UINT64_C(1) << 52) - 1)), 0, 0, 0);
  __m256i column_hi = _mm256_setzero_si256();
#define B58_ADD_LO(row, value)                                                \
  column_lo = _mm256_add_epi64(                                              \
      column_lo, _mm256_mul_epu32(                                           \
          _mm256_set1_epi64x((long long)(value)),                            \
          _mm256_load_si256(                                                 \
              (const __m256i *)(const void *)&B58_W5_ENCODE[(row)][0])))
#define B58_ADD_HI(row, value)                                                \
  column_hi = _mm256_add_epi64(                                              \
      column_hi, _mm256_mul_epu32(                                           \
          _mm256_set1_epi64x((long long)(value)),                            \
          _mm256_load_si256(                                                 \
              (const __m256i *)(const void *)&B58_W5_ENCODE[(row)][4])))
  B58_ADD_LO(2, a2);
  B58_ADD_LO(3, a3);
  B58_ADD_LO(4, a4);
  B58_ADD_LO(5, a5); B58_ADD_HI(5, a5);
  B58_ADD_LO(6, a6); B58_ADD_HI(6, a6);
  B58_ADD_LO(7, a7); B58_ADD_HI(7, a7);
  B58_ADD_LO(8, a8); B58_ADD_HI(8, a8);
  B58_ADD_LO(9, a9); B58_ADD_HI(9, a9);
#undef B58_ADD_LO
#undef B58_ADD_HI

  const b58_raw_columns result = {column_lo, column_hi};
  return result;
}

/* Normalize raw columns into cells 0..7; return cell 8 through top_cell. */
static inline __m256i
b58_finish_cells(b58_raw_columns raw, uint32_t *top_cell) {
  const __m256i column_lo = raw.lo;
  const __m256i column_hi = raw.hi;

  __m256i qlo, qhi, rlo, rhi;
  b58_div_b5_8(column_lo, column_hi, &qlo, &qhi, &rlo, &rhi);

  const __m256i zero = _mm256_setzero_si256();
  __m256i qprev_lo = _mm256_permute4x64_epi64(qlo, 0x90);
  qprev_lo = _mm256_blend_epi32(zero, qprev_lo, 0xfc);
  const __m256i cross = _mm256_permute2x128_si256(qlo, qhi, 0x21);
  const __m256i qprev_hi = _mm256_alignr_epi8(qhi, cross, 8);
  __m256i sum_lo = _mm256_add_epi64(rlo, qprev_lo);
  __m256i sum_hi = _mm256_add_epi64(rhi, qprev_hi);

  const __m256i propagate = _mm256_set1_epi64x((long long)(B58_B5 - 1));
  const __m256i initial_generate_lo =
      _mm256_cmpgt_epi64(sum_lo, propagate);
  const __m256i initial_generate_hi =
      _mm256_cmpgt_epi64(sum_hi, propagate);
  unsigned g = (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(
      initial_generate_lo));
  g |= (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(
           initial_generate_hi)) << 4;
  const __m256i propagate_lo = _mm256_cmpeq_epi64(sum_lo, propagate);
  const __m256i propagate_hi = _mm256_cmpeq_epi64(sum_hi, propagate);
  const __m256i any_propagate =
      _mm256_or_si256(propagate_lo, propagate_hi);
  const unsigned had_propagate = (unsigned)!_mm256_testz_si256(
      any_propagate, any_propagate);
  if (__builtin_expect(had_propagate != 0U, 0)) {
    unsigned p = (unsigned)_mm256_movemask_pd(
        _mm256_castsi256_pd(propagate_lo));
    p |= (unsigned)_mm256_movemask_pd(
             _mm256_castsi256_pd(propagate_hi)) << 4;
    g |= p & (g << 1); p &= p << 1;
    g |= p & (g << 2); p &= p << 2;
    g |= p & (g << 4);
  }
  g &= 0xffU;
  const unsigned carry_in = (g << 1) & 0xffU;
  const __m256i carry_lo = _mm256_cvtepu8_epi64(
      _mm_cvtsi32_si128((int)B58_EXPAND4[carry_in & 15U]));
  const __m256i carry_hi = _mm256_cvtepu8_epi64(
      _mm_cvtsi32_si128((int)B58_EXPAND4[carry_in >> 4]));
  sum_lo = _mm256_add_epi64(sum_lo, carry_lo);
  sum_hi = _mm256_add_epi64(sum_hi, carry_hi);
  const __m256i divisor = _mm256_set1_epi64x((long long)B58_B5);
  __m256i generate_lo = initial_generate_lo;
  __m256i generate_hi = initial_generate_hi;
  if (__builtin_expect(had_propagate != 0U, 0)) {
    generate_lo = _mm256_cmpgt_epi64(sum_lo, propagate);
    generate_hi = _mm256_cmpgt_epi64(sum_hi, propagate);
  }
  sum_lo = _mm256_sub_epi64(
      sum_lo, _mm256_and_si256(generate_lo, divisor));
  sum_hi = _mm256_sub_epi64(
      sum_hi, _mm256_and_si256(generate_hi, divisor));

  *top_cell = (uint32_t)(uint64_t)_mm256_extract_epi64(qhi, 3)
            + ((g >> 7) & 1U);
  const __m256i index = _mm256_setr_epi32(0,2,4,6,0,0,0,0);
  const __m256i low_dwords = _mm256_permutevar8x32_epi32(sum_lo, index);
  const __m256i high_dwords = _mm256_permutevar8x32_epi32(sum_hi, index);
  return _mm256_permute2x128_si256(low_dwords, high_dwords, 0x20);
}

/* Emit a cell vector for the common case where the most-significant input
 * byte is nonzero.  In that domain the canonical skip is exactly 1 or 2. */
static inline size_t
b58_emit_common(__m256i cell, uint32_t top_cell, char output[45],
                int simd_top) {
  const __m256i zero = _mm256_setzero_si256();
  const __m256i top_pair = b58_div195112_u32(cell);
  const __m256i bottom3 = _mm256_sub_epi32(
      cell, _mm256_mullo_epi32(top_pair, _mm256_set1_epi32((int)B58_B3)));
  const __m256i quarter = _mm256_srli_epi32(bottom3, 2);
  const __m256i pair = _mm256_packus_epi32(top_pair, quarter);
  const __m256i first_magic = _mm256_setr_epi16(
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899,
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899);
  const __m256i first_high = _mm256_mulhi_epu16(pair, first_magic);
  const __m256i high = _mm256_blend_epi16(
      first_high, _mm256_srli_epi16(first_high, 9), 0xf0);
  const __m256i first_coefficient = _mm256_setr_epi16(
      58,58,58,58,841,841,841,841,58,58,58,58,841,841,841,841);
  const __m256i first_remainder = _mm256_sub_epi16(
      pair, _mm256_mullo_epi16(high, first_coefficient));
  const __m256i scale = _mm256_setr_epi16(
      1,1,1,1,4,4,4,4,1,1,1,1,4,4,4,4);
  __m256i pair58 = _mm256_mullo_epi16(first_remainder, scale);
  const __m256i low2 =
      _mm256_and_si256(bottom3, _mm256_set1_epi32(3));
  pair58 = _mm256_add_epi16(
      pair58, _mm256_packus_epi32(zero, low2));
  const __m256i middle =
      _mm256_mulhi_epu16(pair58, _mm256_set1_epi16(1130));
  const __m256i low = _mm256_sub_epi16(
      pair58, _mm256_mullo_epi16(middle, _mm256_set1_epi16(58)));

  const __m256i byte_fields = _mm256_packus_epi16(low, middle);
  const __m256i top_field = _mm256_packus_epi16(high, zero);
  const __m256i fields = _mm256_or_si256(
      _mm256_shuffle_epi8(
          byte_fields,
          _mm256_load_si256((const __m256i *)(const void *)B58_FIELDS_A)),
      _mm256_shuffle_epi8(
          top_field,
          _mm256_load_si256((const __m256i *)(const void *)B58_FIELDS_B)));
  const __m256i head = _mm256_or_si256(
      _mm256_shuffle_epi8(
          fields,
          _mm256_load_si256((const __m256i *)(const void *)B58_HEAD_A)),
      _mm256_shuffle_epi8(
          top_field,
          _mm256_load_si256((const __m256i *)(const void *)B58_HEAD_B)));
  const __m256i tail = _mm256_shuffle_epi8(
      fields, _mm256_load_si256((const __m256i *)(const void *)B58_TAIL_A));

  const __m128i head_lo = _mm256_castsi256_si128(head);
  const __m128i head_hi = _mm256_extracti128_si256(head, 1);
  const __m128i tail_lo = _mm256_castsi256_si128(tail);
  const __m128i tail_hi = _mm256_extracti128_si256(tail, 1);
  uint32_t top0;
  uint64_t top_digits;
  if (simd_top) {
    const b58_top_digits top = b58_split_top_simd(top_cell);
    top0 = top.first;
    top_digits = top.packed;
  } else {
    const uint32_t top2 = top_cell / B58_B2;
    const uint32_t top_remainder = top_cell - top2 * B58_B2;
    top0 = top2 / 58U;
    const uint32_t top1 = top2 - top0 * 58U;
    const uint32_t top3 = top_remainder / 58U;
    const uint32_t top4 = top_remainder - top3 * 58U;
    top_digits = ((uint64_t)top0 << 8)
               | ((uint64_t)top1 << 16)
               | ((uint64_t)top3 << 24)
               | ((uint64_t)top4 << 32);
  }
  const __m128i digit0_lo = _mm_or_si128(
      _mm_cvtsi64_si128((long long)top_digits), _mm_slli_si128(head_hi, 5));
  const __m128i digit0_hi = _mm_or_si128(
      _mm_srli_si128(head_hi, 11),
      _mm_or_si128(_mm_slli_si128(tail_hi, 5),
                   _mm_slli_si128(head_lo, 9)));
  const __m128i digit1_lo = _mm_or_si128(
      _mm_srli_si128(head_lo, 7), _mm_slli_si128(tail_lo, 9));
  const __m256i digit0 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(digit0_lo), digit0_hi, 1);
  const __m256i digit1 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(digit1_lo), _mm_setzero_si128(), 1);
  const __m256i ascii0 = b58_map58(digit0);
  const __m256i ascii1 = b58_map58(digit1);
  const __m128i ascii0_lo = _mm256_castsi256_si128(ascii0);
  const __m128i ascii0_hi = _mm256_extracti128_si256(ascii0, 1);
  const __m128i ascii1_lo = _mm256_castsi256_si128(ascii1);
  __m128i output_lo;
  __m128i output_hi;
  unsigned skip;
  if (__builtin_expect(top0 != 0U, 1)) {
    output_lo = _mm_alignr_epi8(ascii0_hi, ascii0_lo, 1);
    output_hi = _mm_alignr_epi8(ascii1_lo, ascii0_hi, 1);
    skip = 1U;
  } else {
    output_lo = _mm_alignr_epi8(ascii0_hi, ascii0_lo, 2);
    output_hi = _mm_alignr_epi8(ascii1_lo, ascii0_hi, 2);
    skip = 2U;
  }
  const __m256i output_vector = _mm256_inserti128_si256(
      _mm256_castsi128_si256(output_lo), output_hi, 1);
  const __m128i output_tail =
      _mm_alignr_epi8(ascii1_lo, ascii0_hi, 13);
  const size_t length = 45U - skip;
  _mm256_storeu_si256((__m256i *)(void *)output, output_vector);
  _mm_storeu_si128((__m128i *)(void *)(output + length - 16U), output_tail);
  output[length] = '\0';
  return length;
}

static __attribute__((noinline)) size_t
b58_emit_general(const uint8_t input[32], __m256i cell, uint32_t top_cell,
                 char output[45]) {
  _Alignas(32) uint8_t ascii[64];
  const __m256i zero = _mm256_setzero_si256();

  /* Eight B5 cells -> five byte fields each.  The r3/3364 operation is
   * performed as floor(floor(r3/4)/841), with the two low bits restored. */
  const __m256i top_pair = b58_div195112_u32(cell);
  const __m256i bottom3 = _mm256_sub_epi32(
      cell, _mm256_mullo_epi32(top_pair, _mm256_set1_epi32((int)B58_B3)));
  const __m256i quarter = _mm256_srli_epi32(bottom3, 2);
  const __m256i pair = _mm256_packus_epi32(top_pair, quarter);
  const __m256i first_magic = _mm256_setr_epi16(
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899,
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899);
  const __m256i first_high = _mm256_mulhi_epu16(pair, first_magic);
  const __m256i high = _mm256_blend_epi16(
      first_high, _mm256_srli_epi16(first_high, 9), 0xf0);
  const __m256i first_coefficient = _mm256_setr_epi16(
      58,58,58,58,841,841,841,841,58,58,58,58,841,841,841,841);
  const __m256i first_remainder = _mm256_sub_epi16(
      pair, _mm256_mullo_epi16(high, first_coefficient));
  const __m256i scale = _mm256_setr_epi16(
      1,1,1,1,4,4,4,4,1,1,1,1,4,4,4,4);
  __m256i pair58 = _mm256_mullo_epi16(first_remainder, scale);
  const __m256i low2 =
      _mm256_and_si256(bottom3, _mm256_set1_epi32(3));
  pair58 = _mm256_add_epi16(
      pair58, _mm256_packus_epi32(zero, low2));
  const __m256i middle =
      _mm256_mulhi_epu16(pair58, _mm256_set1_epi16(1130));
  const __m256i low = _mm256_sub_epi16(
      pair58, _mm256_mullo_epi16(middle, _mm256_set1_epi16(58)));

  const __m256i byte_fields = _mm256_packus_epi16(low, middle);
  const __m256i top_field = _mm256_packus_epi16(high, zero);
  const __m256i fields = _mm256_or_si256(
      _mm256_shuffle_epi8(
          byte_fields,
          _mm256_load_si256((const __m256i *)(const void *)B58_FIELDS_A)),
      _mm256_shuffle_epi8(
          top_field,
          _mm256_load_si256((const __m256i *)(const void *)B58_FIELDS_B)));
  const __m256i head = _mm256_or_si256(
      _mm256_shuffle_epi8(
          fields, _mm256_load_si256(
                      (const __m256i *)(const void *)B58_HEAD_A)),
      _mm256_shuffle_epi8(
          top_field, _mm256_load_si256(
                         (const __m256i *)(const void *)B58_HEAD_B)));
  const __m256i tail = _mm256_shuffle_epi8(
      fields, _mm256_load_si256((const __m256i *)(const void *)B58_TAIL_A));

  const __m128i head_lo = _mm256_castsi256_si128(head);
  const __m128i head_hi = _mm256_extracti128_si256(head, 1);
  const __m128i tail_lo = _mm256_castsi256_si128(tail);
  const __m128i tail_hi = _mm256_extracti128_si256(tail, 1);

  /* The ninth cell is below 58^4, so its first field is always zero. */
  const uint32_t top2 = top_cell / B58_B2;
  const uint32_t top_remainder = top_cell - top2 * B58_B2;
  const uint32_t top0 = b58_div58_u32(top2);
  const uint32_t top1 = top2 - top0 * 58U;
  const uint32_t top3 = b58_div58_u32(top_remainder);
  const uint32_t top4 = top_remainder - top3 * 58U;
  const uint64_t top_digits = ((uint64_t)top0 << 8)
                            | ((uint64_t)top1 << 16)
                            | ((uint64_t)top3 << 24)
                            | ((uint64_t)top4 << 32);
  const __m128i digit0_lo = _mm_or_si128(
      _mm_cvtsi64_si128((long long)top_digits), _mm_slli_si128(head_hi, 5));
  const __m128i digit0_hi = _mm_or_si128(
      _mm_srli_si128(head_hi, 11),
      _mm_or_si128(_mm_slli_si128(tail_hi, 5),
                   _mm_slli_si128(head_lo, 9)));
  const __m128i digit1_lo = _mm_or_si128(
      _mm_srli_si128(head_lo, 7), _mm_slli_si128(tail_lo, 9));
  const __m256i digit0 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(digit0_lo), digit0_hi, 1);
  const __m256i digit1 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(digit1_lo), _mm_setzero_si128(), 1);
  const __m256i ascii0 = b58_map58(digit0);
  const __m256i ascii1 = b58_map58(digit1);

  uint64_t nonzero_digits =
      (uint32_t)~(uint32_t)_mm256_movemask_epi8(
          _mm256_cmpeq_epi8(digit0, zero));
  nonzero_digits |= (uint64_t)(uint32_t)~(uint32_t)_mm256_movemask_epi8(
      _mm256_cmpeq_epi8(digit1, zero)) << 32;
  nonzero_digits &= (UINT64_C(1) << 45) - 1;
  const unsigned first_digit = nonzero_digits
      ? (unsigned)__builtin_ctzll(nonzero_digits) : 45U;

  _mm256_store_si256((__m256i *)(void *)ascii, ascii0);
  _mm256_store_si256((__m256i *)(void *)(ascii + 32), ascii1);

  const __m256i input_vector =
      _mm256_loadu_si256((const __m256i *)(const void *)input);
  const uint32_t nonzero_bytes =
      ~(uint32_t)_mm256_movemask_epi8(
          _mm256_cmpeq_epi8(input_vector, zero));
  const unsigned leading_zero_bytes = nonzero_bytes
      ? (unsigned)__builtin_ctz(nonzero_bytes) : 32U;

  const unsigned skip = first_digit - leading_zero_bytes;
  const size_t length = 45U - skip;
  _mm256_storeu_si256((__m256i *)(void *)output,
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii + skip)));
  _mm_storeu_si128((__m128i *)(void *)(output + length - 16U),
      _mm_loadu_si128((const __m128i *)(const void *)(ascii + 29)));
  output[length] = '\0';
  return length;
}

void
braid58_encode_32x2_avx2(const uint8_t input[2][32],
                         char output[2][45], size_t output_len[2]) {
  const b58_raw_columns raw0 = b58_make_raw_columns(input[0]);
  const b58_raw_columns raw1 = b58_make_raw_columns(input[1]);
  uint32_t top0, top1;
  const __m256i cell0 = b58_finish_cells(raw0, &top0);
  const __m256i cell1 = b58_finish_cells(raw1, &top1);
  output_len[0] = input[0][0] != 0
      ? b58_emit_common(cell0, top0, output[0], 0)
      : b58_emit_general(input[0], cell0, top0, output[0]);
  output_len[1] = input[1][0] != 0
      ? b58_emit_common(cell1, top1, output[1], 0)
      : b58_emit_general(input[1], cell1, top1, output[1]);
}

size_t
braid58_encode_32_avx2(const uint8_t input[32], char output[45]) {
  const b58_raw_columns raw = b58_make_raw_columns(input);
  uint32_t top;
  const __m256i cell = b58_finish_cells(raw, &top);
  return input[0] != 0 ? b58_emit_common(cell, top, output, 1)
                       : b58_emit_general(input, cell, top, output);
}

void
braid58_encode_32x3_avx2(const uint8_t input[3][32],
                         char output[3][45], size_t output_len[3]) {
  const b58_raw_columns raw0 = b58_make_raw_columns(input[0]);
  const b58_raw_columns raw1 = b58_make_raw_columns(input[1]);
  const b58_raw_columns raw2 = b58_make_raw_columns(input[2]);
  uint32_t top0, top1, top2;
  const __m256i cell0 = b58_finish_cells(raw0, &top0);
  const __m256i cell1 = b58_finish_cells(raw1, &top1);
  const __m256i cell2 = b58_finish_cells(raw2, &top2);
  output_len[0] = input[0][0] != 0
      ? b58_emit_common(cell0, top0, output[0], 0)
      : b58_emit_general(input[0], cell0, top0, output[0]);
  output_len[1] = input[1][0] != 0
      ? b58_emit_common(cell1, top1, output[1], 0)
      : b58_emit_general(input[1], cell1, top1, output[1]);
  output_len[2] = input[2][0] != 0
      ? b58_emit_common(cell2, top2, output[2], 0)
      : b58_emit_general(input[2], cell2, top2, output[2]);
}
