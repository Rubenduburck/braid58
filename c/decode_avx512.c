/*
 * AVX-512 Bitcoin Base58 decoder for 32-byte outputs.
 *
 * The 43/44-character path uses a radix-58^4 transform and maps the complete
 * 44-digit frame in one ZMM. Matrix width
 * follows coefficient density: five ZMM products, four YMM products, and a
 * scalar fold for the final two rows.  Short/leading-'1' inputs retain an
 * active-row-skipping ZMM B4 path. Both paths use scalar
 * base-2^32 normalization and failure-atomic canonical commit.
 * Required ISA: AVX2, BMI1 and AVX-512F/DQ/BW/VL/VBMI.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

/* ASCII 0..127 to Bitcoin Base58; invalid entries are 255. */
_Alignas(64) static const uint8_t B58_INV[128] = {
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,  0,  1,  2,  3,  4,  5,  6,  7,  8,255,255,255,255,255,255,
  255,  9, 10, 11, 12, 13, 14, 15, 16,255, 17, 18, 19, 20, 21,255,
   22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,255,255,255,255,255,
  255, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,255, 44, 45, 46,
   47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,255,255,255,255,255,
};

/* Little-endian base-2^32 expansion of (58^4)^(10-row). */
_Alignas(64) static const uint64_t B58_W32_D4[11][8] = {
  {UINT64_C(0),UINT64_C(2959155456),UINT64_C(687255411),
   UINT64_C(3248244966),UINT64_C(2074386530),UINT64_C(3801011509),
   UINT64_C(2650397687),UINT64_C(1277)},
  {UINT64_C(0),UINT64_C(828527888),UINT64_C(3393287680),
   UINT64_C(698399827),UINT64_C(667673988),UINT64_C(4291272133),
   UINT64_C(484895),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(4219425409),UINT64_C(2265142903),
   UINT64_C(4119564573),UINT64_C(1570454940),UINT64_C(184033331),
   UINT64_C(0),UINT64_C(0)},
  {UINT64_C(268435456),UINT64_C(821090699),UINT64_C(2252078569),
   UINT64_C(771698833),UINT64_C(1126979233),UINT64_C(16),
   UINT64_C(0),UINT64_C(0)},
  {UINT64_C(3774873600),UINT64_C(469881767),UINT64_C(649946844),
   UINT64_C(395721298),UINT64_C(6172),UINT64_C(0),UINT64_C(0),
   UINT64_C(0)},
  {UINT64_C(17825792),UINT64_C(2595180627),UINT64_C(3052466824),
   UINT64_C(2342503),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(3779133440),UINT64_C(497183526),UINT64_C(889054070),
   UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(3870756864),UINT64_C(2416622419),UINT64_C(78),
   UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(41853184),UINT64_C(29817),UINT64_C(0),UINT64_C(0),
   UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(11316496),UINT64_C(0),UINT64_C(0),UINT64_C(0),
   UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(1),UINT64_C(0),UINT64_C(0),UINT64_C(0),
   UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
};

/* Source indices that assemble the two overlapping safe loads. */
_Alignas(64) static const uint8_t B58_PERM_44[64] = {
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  52,53,54,55,56,57,58,59,60,61,62,63,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0,
};

_Alignas(64) static const uint8_t B58_PERM_43[64] = {
   0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,
  52,53,54,55,56,57,58,59,60,61,62,63,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0,
};

static inline __m256i
b58_shift_right(__m256i value, unsigned count) {
  const __m256i previous = _mm256_permute2x128_si256(value, value, 0x08);
#define SHIFT_CASE(N) case (N): return _mm256_alignr_epi8(                   \
    value, previous, 16 - (N))
  switch (count) {
  case 0: return value;
  SHIFT_CASE(1); SHIFT_CASE(2); SHIFT_CASE(3); SHIFT_CASE(4);
  SHIFT_CASE(5); SHIFT_CASE(6); SHIFT_CASE(7); SHIFT_CASE(8);
  SHIFT_CASE(9); SHIFT_CASE(10); SHIFT_CASE(11); SHIFT_CASE(12);
  default: return _mm256_setzero_si256();
  }
#undef SHIFT_CASE
}

static inline int
b58_normalize(__m512i raw, size_t leading, uint8_t output[32]) {
  _Alignas(64) uint64_t column[8];
  _Alignas(32) uint32_t word[8];
  _mm512_store_si512((void *)column, raw);
  uint64_t carry = 0;
  for (unsigned i = 0; i < 8; ++i) {
    const uint64_t value = column[i] + carry;
    word[i] = (uint32_t)value;
    carry = value >> 32;
  }
  if (carry) return 0;

  const __m256i words = _mm256_load_si256(
      (const __m256i *)(const void *)word);
  const __m256i rev = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
  const __m256i bswap = _mm256_setr_epi8(
      3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,
      3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
  const __m256i be = _mm256_shuffle_epi8(
      _mm256_permutevar8x32_epi32(words, rev), bswap);

  if (__builtin_expect(leading == 0, 1)) {
    if ((word[7] & UINT32_C(0xff000000)) == 0) return 0;
  } else {
    const uint32_t nonzero = ~(uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(be, _mm256_setzero_si256()));
    const size_t zero_bytes = nonzero ? (size_t)__builtin_ctz(nonzero) : 32U;
    if (leading != zero_bytes) return 0;
  }
  _mm256_storeu_si256((__m256i *)(void *)output, be);
  return 1;
}

#define ZMM_PRODUCT(R) _mm512_mul_epu32(                                    \
    _mm512_set1_epi32((int)cell[(R)]),                                     \
    _mm512_load_si512((const void *)&B58_W32_D4[(R)][0]))

/* Active-row-skipping fallback for short and leading-zero encodings. */
static inline __m512i
b58_matrix_skip(const uint32_t cell[16], unsigned first) {
  __m512i acc = _mm512_setzero_si512();
#define ZROW(R) acc = _mm512_add_epi64(acc, ZMM_PRODUCT(R))
#define FALL __attribute__((fallthrough))
  switch (first) {
  case 0: ZROW(0); FALL; case 1: ZROW(1); FALL;
  case 2: ZROW(2); FALL; case 3: ZROW(3); FALL;
  case 4: ZROW(4); FALL; case 5: ZROW(5); FALL;
  case 6: ZROW(6); FALL; case 7: ZROW(7); FALL;
  case 8: ZROW(8); FALL; case 9: ZROW(9); FALL;
  case 10: ZROW(10); break; default: break;
  }
#undef FALL
#undef ZROW
  return acc;
}

#undef ZMM_PRODUCT

/* Sparse full matrix: 5 wide rows, 4 narrow rows, then cell9*B4+cell10. */
static inline __m512i
b58_matrix_full(const uint32_t cell[16]) {
  __m512i za = _mm512_setzero_si512();
  __m512i zb = _mm512_setzero_si512();
  __m256i ya = _mm256_setzero_si256();
  __m256i yb = _mm256_setzero_si256();
#define ZP(R) _mm512_mul_epu32(                                              \
    _mm512_set1_epi32((int)cell[(R)]),                                      \
    _mm512_load_si512((const void *)&B58_W32_D4[(R)][0]))
#define YP(R) _mm256_mul_epu32(                                              \
    _mm256_set1_epi32((int)cell[(R)]),                                      \
    _mm256_load_si256((const __m256i *)(const void *)&B58_W32_D4[(R)][0]))
  za = _mm512_add_epi64(za, ZP(0));
  zb = _mm512_add_epi64(zb, ZP(1));
  za = _mm512_add_epi64(za, ZP(2));
  zb = _mm512_add_epi64(zb, ZP(3));
  za = _mm512_add_epi64(za, ZP(4));
  ya = _mm256_add_epi64(ya, YP(5));
  yb = _mm256_add_epi64(yb, YP(6));
  ya = _mm256_add_epi64(ya, YP(7));
  yb = _mm256_add_epi64(yb, YP(8));
#undef YP
#undef ZP
  const uint64_t tail =
      (uint64_t)cell[9] * UINT64_C(11316496) + (uint64_t)cell[10];
  __m256i narrow = _mm256_add_epi64(ya, yb);
  narrow = _mm256_add_epi64(
      narrow, _mm256_set_epi64x(0, 0, 0, (long long)tail));
  const __m512i wide = _mm512_add_epi64(za, zb);
  return _mm512_mask_add_epi64(
      wide, (__mmask8)0x0f, wide, _mm512_castsi256_si512(narrow));
}

static inline __m512i
b58_map_two_loads(const char *input, size_t input_len, __m256i *first) {
  *first = _mm256_loadu_si256((const __m256i *)(const void *)input);
  const __m256i last = _mm256_loadu_si256(
      (const __m256i *)(const void *)(input + input_len - 32));
  __m512i ascii = _mm512_castsi256_si512(*first);
  ascii = _mm512_inserti64x4(ascii, last, 1);
  const __m512i mapped = _mm512_permutex2var_epi8(
      _mm512_load_si512((const void *)B58_INV), ascii,
      _mm512_load_si512((const void *)(B58_INV + 64)));
  const __mmask64 bad =
      _mm512_movepi8_mask(ascii) | _mm512_movepi8_mask(mapped);
  return _mm512_mask_mov_epi8(mapped, bad, _mm512_set1_epi8((char)255));
}

__attribute__((noinline)) static int
b58_decode_fallback(const char *input, size_t input_len, uint8_t output[32]) {
  __m256i ch0;
  const __m512i mapped = b58_map_two_loads(input, input_len, &ch0);
  if (_mm512_movepi8_mask(mapped) != 0) return 0;

  size_t leading = 0;
  if (input[0] == '1') {
    const uint32_t different = ~(uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(ch0, _mm256_set1_epi8('1')));
    if (different) leading = (size_t)__builtin_ctz(different);
    else {
      leading = 32;
      while (leading < input_len && input[leading] == '1') ++leading;
    }
  }

  const __m256i d0 = _mm512_castsi512_si256(mapped);
  const __m256i d1 = _mm512_extracti64x4_epi64(mapped, 1);
  __m256i head;
  if (input_len >= 43) {
    if (input_len == 44) head = d0;
    else {
      const __m256i previous =
          _mm256_permute2x128_si256(d0, d0, 0x08);
      head = _mm256_alignr_epi8(d0, previous, 15);
    }
  } else {
    head = b58_shift_right(d0, (unsigned)(44U - input_len));
  }
  __m128i tail128 = _mm256_extracti128_si256(d1, 1);
  tail128 = _mm_srli_si128(tail128, 4);
  const __m256i tail = _mm256_inserti128_si256(
      _mm256_setzero_si256(), tail128, 0);
  __m512i digits = _mm512_castsi256_si512(head);
  digits = _mm512_inserti64x4(digits, tail, 1);
  const __m512i cells = _mm512_madd_epi16(
      _mm512_maddubs_epi16(digits, _mm512_set1_epi16(0x013a)),
      _mm512_set1_epi32(0x00010d24));
  _Alignas(64) uint32_t cell[16];
  _mm512_store_si512((void *)cell, cells);
  const unsigned first = (unsigned)((44U - input_len + leading) / 4U);
  return b58_normalize(b58_matrix_skip(cell, first), leading, output);
}

__attribute__((noinline)) int
braid58_decode_32_avx512(const char *input, size_t input_len,
                         uint8_t output[32]) {
  if (!input || !output || input_len < 32 || input_len > 44) return 0;
  if (__builtin_expect(input_len < 43 || input[0] == '1', 0))
    return b58_decode_fallback(input, input_len, output);

  __m256i ch0;
  const __m512i mapped = b58_map_two_loads(input, input_len, &ch0);
  (void)ch0;
  if (_mm512_movepi8_mask(mapped) != 0) return 0;

  const __mmask64 mask = input_len == 44
      ? (__mmask64)((UINT64_C(1) << 44) - 1)
      : (__mmask64)(((UINT64_C(1) << 43) - 1) << 1);
  const __m512i index = _mm512_load_si512((const void *)(
      input_len == 44 ? B58_PERM_44 : B58_PERM_43));
  const __m512i digits =
      _mm512_maskz_permutexvar_epi8(mask, index, mapped);
  const __m512i cells = _mm512_madd_epi16(
      _mm512_maddubs_epi16(digits, _mm512_set1_epi16(0x013a)),
      _mm512_set1_epi32(0x00010d24));
  _Alignas(64) uint32_t cell[16];
  _mm512_store_si512((void *)cell, cells);
  return b58_normalize(b58_matrix_full(cell), 0, output);
}

/*
 * Batch entry points call the selected ZMM-B4 body. The retired YMM-B4 and
 * B6/IFMA decoders are not referenced.
 */
unsigned
braid58_decode_32x2_avx512(const char *const input[2],
                           const size_t input_len[2],
                           uint8_t output[2][32]) {
  const unsigned ok0 = (unsigned)braid58_decode_32_avx512(
      input[0], input_len[0], output[0]);
  const unsigned ok1 = (unsigned)braid58_decode_32_avx512(
      input[1], input_len[1], output[1]);
  return ok0 | (ok1 << 1);
}

unsigned
braid58_decode_32x3_avx512(const char *const input[3],
                           const size_t input_len[3],
                           uint8_t output[3][32]) {
  const unsigned ok0 = (unsigned)braid58_decode_32_avx512(
      input[0], input_len[0], output[0]);
  const unsigned ok1 = (unsigned)braid58_decode_32_avx512(
      input[1], input_len[1], output[1]);
  const unsigned ok2 = (unsigned)braid58_decode_32_avx512(
      input[2], input_len[2], output[2]);
  return ok0 | (ok1 << 1) | (ok2 << 2);
}
