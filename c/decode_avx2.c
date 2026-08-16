/*
 * Strict-AVX2 fixed-width Bitcoin Base58 decoder for 32-byte values.
 *
 * The input is left-padded to 44 digits and folded into eleven radix-58^4
 * cells with vpmaddubsw/vpmaddwd.  A transposed radix-conversion matrix then
 * produces eight base-2^32 columns with 16 vpmuludq groups.  Leading zero
 * cells select an active-row suffix.  A short scalar carry chain normalizes
 * the columns before the sole output store.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

/* ASCII 0..127 to Bitcoin Base58; invalid entries are 255. */
_Alignas(32) static const uint8_t B58_INV[128] = {
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,  0,  1,  2,  3,  4,  5,  6,  7,  8,255,255,255,255,255,255,
  255,  9, 10, 11, 12, 13, 14, 15, 16,255, 17, 18, 19, 20, 21,255,
   22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,255,255,255,255,255,
  255, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,255, 44, 45, 46,
   47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,255,255,255,255,255,
};

/*
 * Row i is the little-endian base-2^32 expansion of (58^4)^(10-i).
 * Qword storage puts each uint32 coefficient in vpmuludq's low dword.
 */
_Alignas(32) static const uint64_t B58_W32_D4[11][8] = {
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

static inline __m256i
b58_decode_map_ascii(__m256i ch) {
  const __m256i nibble = _mm256_set1_epi8(15);
  const __m256i lo = _mm256_and_si256(ch, nibble);
  const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(ch, 4), nibble);
  const __m256i u = _mm256_sub_epi8(hi, _mm256_set1_epi8(3));
  const __m256i m0 = _mm256_slli_epi16(u, 7);
  const __m256i m1 = _mm256_slli_epi16(u, 6);
  const __m256i m2 = _mm256_slli_epi16(u, 5);
#define DTAB(H) _mm256_broadcastsi128_si256(_mm_load_si128(                 \
    (const __m128i *)(const void *)(B58_INV + 16U * (H))))
  const __m256i t3 = _mm256_shuffle_epi8(DTAB(3), lo);
  const __m256i t4 = _mm256_shuffle_epi8(DTAB(4), lo);
  const __m256i t5 = _mm256_shuffle_epi8(DTAB(5), lo);
  const __m256i t6 = _mm256_shuffle_epi8(DTAB(6), lo);
  const __m256i t7 = _mm256_shuffle_epi8(DTAB(7), lo);
#undef DTAB
  const __m256i a = _mm256_blendv_epi8(t3, t4, m0);
  const __m256i b = _mm256_blendv_epi8(t5, t6, m0);
  __m256i value = _mm256_blendv_epi8(a, b, m1);
  value = _mm256_blendv_epi8(value, t7, m2);
  const __m256i below = _mm256_cmpgt_epi8(_mm256_set1_epi8(3), hi);
  const __m256i above = _mm256_cmpgt_epi8(hi, _mm256_set1_epi8(7));
  return _mm256_or_si256(value, _mm256_or_si256(below, above));
}

/* Shift right across the full 256-bit vector, inserting zero bytes. */
static inline __m256i
b58_decode_shift_right(__m256i value, unsigned count) {
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

/* Map, validate, left-pad to 44 digits, and fold four digits per cell. */
static inline size_t
b58_decode_map_parse_b4(const char *input, size_t input_len,
                        uint32_t cell[16], int *valid) {
  const __m256i ch0 = _mm256_loadu_si256(
      (const __m256i *)(const void *)input);
  const __m256i ch1 = _mm256_loadu_si256(
      (const __m256i *)(const void *)(input + input_len - 32));
  const __m256i d0 = b58_decode_map_ascii(ch0);
  const __m256i d1 = b58_decode_map_ascii(ch1);
  *valid = _mm256_movemask_epi8(_mm256_or_si256(d0, d1)) == 0;

  size_t leading = 0;
  if (__builtin_expect(input[0] == '1', 0)) {
    const uint32_t different = ~(uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(ch0, _mm256_set1_epi8('1')));
    if (different) leading = (size_t)__builtin_ctz(different);
    else {
      leading = 32;
      while (leading < input_len && input[leading] == '1') ++leading;
    }
  }

  __m256i head;
  if (__builtin_expect(input_len >= 43, 1)) {
    if (input_len == 44) head = d0;
    else {
      const __m256i previous =
          _mm256_permute2x128_si256(d0, d0, 0x08);
      head = _mm256_alignr_epi8(d0, previous, 15);
    }
  } else {
    head = b58_decode_shift_right(d0, (unsigned)(44U - input_len));
  }

  __m128i tail128 = _mm256_extracti128_si256(d1, 1);
  tail128 = _mm_srli_si128(tail128, 4);
  const __m256i tail = _mm256_inserti128_si256(
      _mm256_setzero_si256(), tail128, 0);
  const __m256i c2 = _mm256_set1_epi16(0x013a);
  const __m256i c4 = _mm256_set1_epi32(0x00010d24);
  _mm256_store_si256((__m256i *)(void *)cell,
      _mm256_madd_epi16(_mm256_maddubs_epi16(head, c2), c4));
  _mm256_store_si256((__m256i *)(void *)(cell + 8),
      _mm256_madd_epi16(_mm256_maddubs_epi16(tail, c2), c4));
  return leading;
}

/* Fixed scratch makes each matrix product immediately feed its accumulator. */
static inline __m256i
b58_decode_mac(__m256i acc, __m256i digit, const __m256i *weight) {
  __asm__ volatile(
      "vpmuludq %2,%1,%%ymm15\n\t"
      "vpaddq %%ymm15,%0,%0"
      : "+x"(acc) : "x"(digit), "m"(*weight) : "ymm15");
  return acc;
}

#define QROW2(R) do {                                                        \
  const __m256i d = _mm256_set1_epi32((int)cell[(R)]);                      \
  lo = b58_decode_mac(lo, d, (const __m256i *)(const void *)                \
      &B58_W32_D4[(R)][0]);                                                 \
  hi = b58_decode_mac(hi, d, (const __m256i *)(const void *)                \
      &B58_W32_D4[(R)][4]);                                                 \
} while (0)
#define QROW1(R) do {                                                        \
  const __m256i d = _mm256_set1_epi32((int)cell[(R)]);                      \
  lo = b58_decode_mac(lo, d, (const __m256i *)(const void *)                \
      &B58_W32_D4[(R)][0]);                                                 \
} while (0)

static inline void
b58_decode_matrix_b4(const uint32_t cell[16], __m256i raw[2]) {
  __m256i lo = _mm256_setzero_si256(), hi = lo;
  QROW2(0); QROW2(1); QROW2(2); QROW2(3); QROW2(4);
  QROW1(5); QROW1(6); QROW1(7); QROW1(8); QROW1(9); QROW1(10);
  raw[0] = lo;
  raw[1] = hi;
}

__attribute__((always_inline)) static inline void
b58_decode_matrix_b4_skip(const uint32_t cell[16], unsigned first,
                          __m256i raw[2]) {
  __m256i lo = _mm256_setzero_si256(), hi = lo;
#define FALL __attribute__((fallthrough))
  switch (first) {
  case 0: QROW2(0); FALL; case 1: QROW2(1); FALL;
  case 2: QROW2(2); FALL; case 3: QROW2(3); FALL;
  case 4: QROW2(4); FALL; case 5: QROW1(5); FALL;
  case 6: QROW1(6); FALL; case 7: QROW1(7); FALL;
  case 8: QROW1(8); FALL; case 9: QROW1(9); FALL;
  case 10: QROW1(10); break; default: break;
  }
#undef FALL
  raw[0] = lo;
  raw[1] = hi;
}

#undef QROW1
#undef QROW2

#define X2MAC(A, B, DA, DB, W) do {                                        \
  const __m256i weight = _mm256_load_si256(                                \
      (const __m256i *)(const void *)(W));                                  \
  __asm__ volatile(                                                         \
      "vpmuludq %4,%2,%%ymm14\n\t"                                      \
      "vpmuludq %4,%3,%%ymm15\n\t"                                      \
      "vpaddq %%ymm14,%0,%0\n\t"                                        \
      "vpaddq %%ymm15,%1,%1"                                             \
      : "+x"(A), "+x"(B) : "x"(DA), "x"(DB), "x"(weight)             \
      : "ymm14", "ymm15");                                              \
} while (0)
#define X2ROW2(R) do {                                                       \
  const __m256i da = _mm256_set1_epi32((int)cell0[(R)]);                    \
  const __m256i db = _mm256_set1_epi32((int)cell1[(R)]);                    \
  X2MAC(a0, b0, da, db, &B58_W32_D4[(R)][0]);                               \
  X2MAC(a1, b1, da, db, &B58_W32_D4[(R)][4]);                               \
} while (0)
#define X2ROW1(R) do {                                                       \
  const __m256i da = _mm256_set1_epi32((int)cell0[(R)]);                    \
  const __m256i db = _mm256_set1_epi32((int)cell1[(R)]);                    \
  X2MAC(a0, b0, da, db, &B58_W32_D4[(R)][0]);                               \
} while (0)

static inline void
b58_decode_matrix_b4_x2(const uint32_t cell0[16],
                        const uint32_t cell1[16], unsigned first,
                        __m256i raw0[2], __m256i raw1[2]) {
  __m256i a0 = _mm256_setzero_si256(), a1 = a0, b0 = a0, b1 = a0;
#define FALL __attribute__((fallthrough))
  switch (first) {
  case 0: X2ROW2(0); FALL; case 1: X2ROW2(1); FALL;
  case 2: X2ROW2(2); FALL; case 3: X2ROW2(3); FALL;
  case 4: X2ROW2(4); FALL; case 5: X2ROW1(5); FALL;
  case 6: X2ROW1(6); FALL; case 7: X2ROW1(7); FALL;
  case 8: X2ROW1(8); FALL; case 9: X2ROW1(9); FALL;
  case 10: X2ROW1(10); break; default: break;
  }
#undef FALL
  raw0[0] = a0;
  raw0[1] = a1;
  raw1[0] = b0;
  raw1[1] = b1;
}

#undef X2ROW1
#undef X2ROW2
#undef X2MAC

/* Normalize eight little-endian base-2^32 columns and commit atomically. */
static inline int
b58_decode_normalize_serial(const __m256i raw[2], size_t leading,
                            uint8_t output[32]) {
  _Alignas(32) uint64_t column[8];
  _Alignas(32) uint32_t word[8];
  _mm256_store_si256((__m256i *)(void *)column, raw[0]);
  _mm256_store_si256((__m256i *)(void *)(column + 4), raw[1]);
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

static inline int
b58_decode_32_core(const char *input, size_t input_len, uint8_t output[32]) {
  if (!input || !output || input_len < 32 || input_len > 44) return 0;
  int valid;
  _Alignas(32) uint32_t cell[16];
  const size_t leading = b58_decode_map_parse_b4(
      input, input_len, cell, &valid);
  if (!valid) return 0;
  __m256i raw[2];
  const unsigned zero_cells = (unsigned)((44U - input_len + leading) / 4U);
  if (__builtin_expect(zero_cells == 0, 1))
    b58_decode_matrix_b4(cell, raw);
  else
    b58_decode_matrix_b4_skip(cell, zero_cells, raw);
  return b58_decode_normalize_serial(raw, leading, output);
}

int
braid58_decode_32_avx2(const char *input, size_t input_len,
                       uint8_t output[32]) {
  return b58_decode_32_core(input, input_len, output);
}

__attribute__((noinline, cold)) static unsigned
b58_decode_32x2_fallback(const char *input0, size_t len0,
                         uint8_t output0[32],
                         const char *input1, size_t len1,
                         uint8_t output1[32]) {
  unsigned mask = 0;
  if (b58_decode_32_core(input0, len0, output0)) mask |= 1U;
  if (b58_decode_32_core(input1, len1, output1)) mask |= 2U;
  return mask;
}

unsigned
braid58_decode_32x2_avx2(const char *input0, size_t len0,
                         uint8_t output0[32],
                         const char *input1, size_t len1,
                         uint8_t output1[32]) {
  if (__builtin_expect(!input0 || !input1 || !output0 || !output1 ||
      len0 < 32 || len0 > 44 || len1 < 32 || len1 > 44, 0))
    return b58_decode_32x2_fallback(
        input0, len0, output0, input1, len1, output1);

  _Alignas(32) uint32_t cell0[16], cell1[16];
  int valid0, valid1;
  const size_t leading0 = b58_decode_map_parse_b4(
      input0, len0, cell0, &valid0);
  const size_t leading1 = b58_decode_map_parse_b4(
      input1, len1, cell1, &valid1);
  if (__builtin_expect(!valid0 || !valid1, 0))
    return b58_decode_32x2_fallback(
        input0, len0, output0, input1, len1, output1);

  const unsigned first0 = (unsigned)((44U - len0 + leading0) / 4U);
  const unsigned first1 = (unsigned)((44U - len1 + leading1) / 4U);
  const unsigned first = first0 < first1 ? first0 : first1;
  __m256i raw0[2], raw1[2];
  b58_decode_matrix_b4_x2(cell0, cell1, first, raw0, raw1);

  unsigned mask = 0;
  if (b58_decode_normalize_serial(raw0, leading0, output0)) mask |= 1U;
  if (b58_decode_normalize_serial(raw1, leading1, output1)) mask |= 2U;
  return mask;
}
