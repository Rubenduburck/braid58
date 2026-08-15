/*
   Braid58: fixed-32-byte Bitcoin Base58 AVX-512 encoder.

   This is an independently derived AVX-512 research kernel.  It interprets
   the input as one unsigned, big-endian 256-bit integer and converts it via

       10 radix-2^26 chunks -> 8 radix-58^6 cells -> 48 radix-58 digits.

   Required ISA: AVX2 and AVX-512 F/DQ/BW/VL/IFMA/VBMI.  The public API
   dispatches here only on supported CPUs.
*/

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define BRAID58_TARGET                                                        \
  __attribute__((target("avx2,avx512f,avx512dq,avx512bw,avx512vl,"          \
                        "avx512ifma,avx512vbmi")))
#else
#define BRAID58_TARGET
#endif

enum { CHUNK_MASK = (1U << 26) - 1U };

static const uint64_t RADIX_B = UINT64_C(38068692544); /* 58^6 */
static const uint64_t RADIX_M = UINT64_C(195112);      /* 58^3 */

/*
   Row i contains the eight little-endian radix-B digits of 2^(26*i).
   Rows zero and one only affect output column zero and are folded directly
   into its accumulator, leaving eight vector multiplies for rows 2..9.
*/
static const uint64_t W6[10][8] __attribute__((aligned(64))) = {
  { UINT64_C(1),           0,                    0,                    0,                    0,                    0,                   0,                0 },
  { UINT64_C(67108864),    0,                    0,                    0,                    0,                    0,                   0,                0 },
  { UINT64_C(35230722752), UINT64_C(118301),     0,                    0,                    0,                    0,                   0,                0 },
  { UINT64_C(19472320256), UINT64_C(20819776900),UINT64_C(208),       0,                    0,                    0,                   0,                0 },
  { UINT64_C(12663192832), UINT64_C(18874684076),UINT64_C(13995345565),0,                   0,                    0,                   0,                0 },
  { UINT64_C(36841335040), UINT64_C(27559759484),UINT64_C(32157257677),UINT64_C(24671499),   0,                    0,                   0,                0 },
  { UINT64_C(35502351808), UINT64_C(32134872081),UINT64_C(17027758953),UINT64_C(30820324005),UINT64_C(43491),      0,                   0,                0 },
  { UINT64_C(249488768),   UINT64_C(11869408826),UINT64_C(19667255935),UINT64_C(21282160635),UINT64_C(25465302058),UINT64_C(76),       0,                0 },
  { UINT64_C(30339540544), UINT64_C(34170182847),UINT64_C(37217747693),UINT64_C(8289492547), UINT64_C(29132788384),UINT64_C(5145164816),0,               0 },
  { UINT64_C(5170572032),  UINT64_C(19969761524),UINT64_C(27888303054),UINT64_C(14786692691),UINT64_C(34727737816),UINT64_C(2939016745),UINT64_C(9070082),0 }
};

/* Gather chunks a2..a9 as eight little-endian 64-bit windows.  Index 32
   selects the zeroed high half when the top window crosses bit 255. */
static const uint8_t GATHER26_BE[64] __attribute__((aligned(64))) = {
  25,24,23,22,21,20,19,18, 22,21,20,19,18,17,16,15,
  18,17,16,15,14,13,12,11, 15,14,13,12,11,10, 9, 8,
  12,11,10, 9, 8, 7, 6, 5,  9, 8, 7, 6, 5, 4, 3, 2,
   5, 4, 3, 2, 1, 0,32,32,  2, 1, 0,32,32,32,32,32
};

static const uint64_t SHIFT26_BE[8] __attribute__((aligned(64))) = {
  4,6,0,2,4,6,0,2
};

/* Big-endian order of all six digits in cells 7..0.  The source field layout
   is [lead digits][middle digits][low digits] for the two 58^3 halves. */
static const uint8_t ORDER6_CANON[64] __attribute__((aligned(64))) = {
   7,23,39,15,31,47, 6,22,38,14,30,46,
   5,21,37,13,29,45, 4,20,36,12,28,44,
   3,19,35,11,27,43, 2,18,34,10,26,42,
   1,17,33, 9,25,41, 0,16,32, 8,24,40,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const uint64_t Q_PREV_INDEX[8] __attribute__((aligned(64))) = {
  0,0,1,2,3,4,5,6
};

static const uint8_t BITCOIN_ALPHABET[64] __attribute__((aligned(64))) =
  "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static const uint8_t IOTA64[64] __attribute__((aligned(64))) = {
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
  48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
};

/* Convert the input to eight normalized little-endian radix-58^6 cells. */
BRAID58_TARGET static inline __m512i
radix6_cells(const uint8_t input[static 32]) {
  /* a0+a1*2^26 is exactly the low 52 bits and only affects column zero. */
  uint64_t low52;
  memcpy(&low52, input + 24, sizeof(low52));
  low52 = __builtin_bswap64(low52) & ((UINT64_C(1) << 52) - 1U);

  const __m256i input256 = _mm256_loadu_si256((const __m256i *)(const void *)input);
  const __m512i source = _mm512_zextsi256_si512(input256);
  __m512i chunks = _mm512_permutexvar_epi8(
      _mm512_load_si512((const void *)GATHER26_BE), source);
  chunks = _mm512_and_si512(
      _mm512_srlv_epi64(
          chunks, _mm512_load_si512((const void *)SHIFT26_BE)),
      _mm512_set1_epi64(CHUNK_MASK));

  uint64_t a[8] __attribute__((aligned(64)));
  _mm512_store_si512((void *)a, chunks);

  __m512i columns = _mm512_mask_set1_epi64(
      _mm512_setzero_si512(), (__mmask8)1, (long long)low52);
  for (unsigned i = 0; i < 8; ++i) {
    columns = _mm512_add_epi64(
        columns,
        _mm512_mullo_epi64(
            _mm512_set1_epi64((long long)a[i]),
            _mm512_load_si512((const void *)W6[i + 2])));
  }

  /*
     Estimate all floor(columns/B) quotients together.  Embedded RN-SAE makes
     the operation independent of the caller's floating-point environment.
     The approximation is within one; the two integer masks make it exact.
  */
  const __m512i bv = _mm512_set1_epi64((long long)RADIX_B);
  const __m512d as_double = _mm512_cvt_roundepu64_pd(
      columns, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
  const __m512d quotient_double = _mm512_mul_round_pd(
      as_double, _mm512_set1_pd(1.0 / 38068692544.0),
      _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
  __m512i q = _mm512_cvt_roundpd_epu64(
      quotient_double, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);

  __m512i product = _mm512_mullo_epi64(q, bv);
  const __mmask8 too_high =
      _mm512_cmp_epu64_mask(product, columns, _MM_CMPINT_GT);
  const __m512i one = _mm512_set1_epi64(1);
  q = _mm512_mask_sub_epi64(q, too_high, q, one);
  product = _mm512_mask_sub_epi64(product, too_high, product, bv);

  __m512i r = _mm512_sub_epi64(columns, product);
  const __mmask8 too_low =
      _mm512_cmp_epu64_mask(r, bv, _MM_CMPINT_NLT);
  q = _mm512_mask_add_epi64(q, too_low, q, one);
  r = _mm512_mask_sub_epi64(r, too_low, r, bv);

  /* Add each raw quotient to the next cell. */
  const __m512i q_previous = _mm512_maskz_permutexvar_epi64(
      (__mmask8)0xfe,
      _mm512_load_si512((const void *)Q_PREV_INDEX), q);
  __m512i sum = _mm512_add_epi64(r, q_previous);

  /*
     Each sum is below B+2^29, so normalization emits at most one carry.
     G marks an unconditional carry and P marks B-1, which propagates an
     incoming carry.  This three-stage bit-prefix scan resolves all eight
     lanes without a scalar carry chain.
  */
  uint32_t g = (uint32_t)_mm512_cmp_epu64_mask(sum, bv, _MM_CMPINT_NLT);
  uint32_t p = (uint32_t)_mm512_cmpeq_epi64_mask(
      sum, _mm512_set1_epi64((long long)(RADIX_B - 1U)));
  g |= p & (g << 1);
  p &= p << 1;
  g |= p & (g << 2);
  p &= p << 2;
  g |= p & (g << 4);
  g &= 0xffU;

  const __mmask8 carry_in = (__mmask8)((g << 1) & 0xffU);
  sum = _mm512_mask_add_epi64(sum, carry_in, sum, one);
  return _mm512_mask_sub_epi64(sum, (__mmask8)g, sum, bv);
}

/* Turn eight radix-58^6 cells into 48 big-endian binary digit values. */
BRAID58_TARGET static inline __m512i
radix6_digits(__m512i cells) {
  /* Split B=(58^3)^2.  The IFMA reciprocal produces the exact quotient for
     cells<B after the fixed 12-bit scale adjustment. */
  const __m512i q3 = _mm512_srli_epi64(
      _mm512_madd52hi_epu64(
          _mm512_setzero_si512(), cells,
          _mm512_set1_epi64(INT64_C(94544385141404))),
      12);
  const __m512i r3 = _mm512_sub_epi64(
      cells,
      _mm512_mullo_epi64(q3, _mm512_set1_epi64((long long)RADIX_M)));

  /* Pack the high and low 58^3 halves as sixteen 32-bit lanes. */
  const __m256i q3d = _mm512_cvtepi64_epi32(q3);
  const __m256i r3d = _mm512_cvtepi64_epi32(r3);
  __m512i v3 = _mm512_castsi256_si512(q3d);
  v3 = _mm512_inserti64x4(v3, r3d, 1);

  /* Split each 58^3 value into one leading digit and one 58^2 value. */
  const __m512i magic3364 = _mm512_set1_epi64(INT64_C(40855813));
  const __m512i qe = _mm512_srli_epi64(
      _mm512_mul_epu32(v3, magic3364), 37);
  const __m512i qo = _mm512_srli_epi64(
      _mm512_mul_epu32(_mm512_srli_epi64(v3, 32), magic3364), 37);
  const __m512i q1 = _mm512_mask_blend_epi32(
      (__mmask16)0xaaaa, qe, _mm512_slli_epi64(qo, 32));
  const __m512i r2 = _mm512_sub_epi32(
      v3, _mm512_mullo_epi32(q1, _mm512_set1_epi32(3364)));
  const __m256i lead16 = _mm512_cvtepi32_epi16(q1);
  const __m256i pair16 = _mm512_cvtepi32_epi16(r2);

  /* Split each 58^2 value into its final two digits. */
  const __m256i mid16 = _mm256_mulhi_epu16(
      pair16, _mm256_set1_epi16(1130));
  const __m256i low16 = _mm256_sub_epi16(
      pair16, _mm256_mullo_epi16(mid16, _mm256_set1_epi16(58)));
  const __m128i lead8 = _mm256_cvtepi16_epi8(lead16);
  const __m128i mid8  = _mm256_cvtepi16_epi8(mid16);
  const __m128i low8  = _mm256_cvtepi16_epi8(low16);

  __m512i fields = _mm512_castsi128_si512(lead8);
  fields = _mm512_inserti32x4(fields, mid8, 1);
  fields = _mm512_inserti32x4(fields, low8, 2);
  return _mm512_permutexvar_epi8(
      _mm512_load_si512((const void *)ORDER6_CANON), fields);
}

/*
   Encode exactly 32 bytes.  `output` must provide 45 bytes.  The returned
   length excludes the trailing NUL.  The function implements Bitcoin's full
   leading-zero-byte rule, including the all-zero input.
*/
BRAID58_TARGET size_t
braid58_encode_32_avx512(const uint8_t input[static 32],
                         char output[static BRAID58_ENCODED_32_CAPACITY]) {
  const __m256i input256 =
      _mm256_loadu_si256((const __m256i *)(const void *)input);
  const uint32_t zero_mask = (uint32_t)_mm256_movemask_epi8(
      _mm256_cmpeq_epi8(input256, _mm256_setzero_si256()));
  const uint32_t nonzero_mask = ~zero_mask;
  const unsigned leading_zero_bytes =
      nonzero_mask ? (unsigned)__builtin_ctz(nonzero_mask) : 32U;

  const __m512i digits = radix6_digits(radix6_cells(input));
  uint64_t nonzero_digits = (uint64_t)_mm512_cmpneq_epi8_mask(
      digits, _mm512_setzero_si512());
  nonzero_digits &= (UINT64_C(1) << 48) - 1U;
  const unsigned first_digit = nonzero_digits
      ? (unsigned)__builtin_ctzll(nonzero_digits)
      : 48U;
  const __m512i ascii = _mm512_permutexvar_epi8(
      digits, _mm512_load_si512((const void *)BITCOIN_ALPHABET));

  /* Starting `leading_zero_bytes` positions before the first significant
     numeric digit automatically retains exactly the required Base58 '1'
     prefix.  Fixed-32 encodings are always at least 32 bytes, so a 32-byte
     head store plus an overlapping 16-byte tail store covers the result. */
  const unsigned skip = first_digit - leading_zero_bytes;
  const size_t length = 48U - skip;
  const __m512i shift = _mm512_add_epi8(
      _mm512_load_si512((const void *)IOTA64),
      _mm512_set1_epi8((char)skip));
  const __m512i start = _mm512_permutexvar_epi8(shift, ascii);
  _mm256_storeu_si256((__m256i *)(void *)output,
                      _mm512_castsi512_si256(start));
  _mm_storeu_si128((__m128i *)(void *)(output + length - 16U),
                   _mm512_extracti32x4_epi32(ascii, 2));
  output[length] = '\0';
  return length;
}
