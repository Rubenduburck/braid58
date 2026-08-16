/*
 * AVX-512 fixed-64 Bitcoin Base58 decoder.
 *
 * Radix-58^4 input cells are transformed to radix-2^32 output words. The Zen
 * 5 schedule uses:
 *
 *   - two safe 64-byte loads and VBMI table mapping cover 64..88 characters;
 *   - two ZMM packed multiply-add folds form all twenty-two B4 cells;
 *   - dense matrix rows use ZMM products, sparse rows use YMM products, and
 *     the final two rows collapse to one scalar multiply-add;
 *   - a 16-lane AVX-512 carry-lookahead normalizes to base 2^32;
 *   - one VBMI byte permutation emits the big-endian 64-byte result.
 *
 * Short and leading-'1' inputs use a fall-through active-row schedule. Output
 * is stored after overflow, width, and canonical-leading-zero validation.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

#define B58_B4 UINT32_C(11316496)

/* ASCII 0..127 to the Bitcoin Base58 alphabet; 255 is invalid. */
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

_Alignas(64) static const uint8_t B58_IOTA[64] = {
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
  48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
};

/* Insert one zero byte before a 63-byte head. */
_Alignas(64) static const uint8_t B58_HEAD_87[64] = {
   0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,
  31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,
  47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,
};

/* The second safe load always contributes its final 24 bytes. */
_Alignas(64) static const uint8_t B58_TAIL_24[64] = {
  40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,
  56,57,58,59,60,61,62,63,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0,
};

_Alignas(64) static const uint8_t B58_REVERSE_64[64] = {
  63,62,61,60,59,58,57,56,55,54,53,52,51,50,49,48,
  47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,
  31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,
  15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
};

/* Row r is (58^4)^(21-r), little-endian in radix 2^32. */
_Alignas(64) static const uint64_t B58_W32_B4[22][16] = {
  {UINT64_C(0),UINT64_C(0),UINT64_C(1360003072),UINT64_C(470871430),UINT64_C(3472963817),UINT64_C(1679150309),UINT64_C(1323148469),UINT64_C(3768780227),UINT64_C(4157684448),UINT64_C(980525252),UINT64_C(2634868627),UINT64_C(942334800),UINT64_C(513644156),UINT64_C(2446686764),UINT64_C(3618591280),UINT64_C(4300)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(104923136),UINT64_C(669472826),UINT64_C(2597060712),UINT64_C(1695615427),UINT64_C(3960476767),UINT64_C(3577221846),UINT64_C(1062993857),UINT64_C(2005415586),UINT64_C(2618421812),UINT64_C(1023671068),UINT64_C(4128706713),UINT64_C(1882780341),UINT64_C(1632305),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(3892776960),UINT64_C(3975052618),UINT64_C(2719044352),UINT64_C(2025890617),UINT64_C(1845212165),UINT64_C(2579102806),UINT64_C(1291695502),UINT64_C(4087920866),UINT64_C(3474734562),UINT64_C(2129066210),UINT64_C(927115866),UINT64_C(619511417),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(1552130304),UINT64_C(571574869),UINT64_C(1461655571),UINT64_C(479444551),UINT64_C(1788203303),UINT64_C(479958106),UINT64_C(2609356203),UINT64_C(4085218026),UINT64_C(445164250),UINT64_C(2758330002),UINT64_C(3195895997),UINT64_C(54),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(1153170704),UINT64_C(163928457),UINT64_C(1397771950),UINT64_C(2713902272),UINT64_C(2182049262),UINT64_C(970086865),UINT64_C(3738990996),UINT64_C(3303319266),UINT64_C(2956193314),UINT64_C(490578553),UINT64_C(20777),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(3969180929),UINT64_C(1340492553),UINT64_C(1739183727),UINT64_C(105569977),UINT64_C(1767257366),UINT64_C(1739519195),UINT64_C(3874617455),UINT64_C(3771111571),UINT64_C(1793220428),UINT64_C(7885570),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(268435456),UINT64_C(2209946163),UINT64_C(3495303162),UINT64_C(486575164),UINT64_C(1447250220),UINT64_C(339767049),UINT64_C(112778334),UINT64_C(3862831115),UINT64_C(383623235),UINT64_C(2992822783),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(1627389952),UINT64_C(3818589354),UINT64_C(681406703),UINT64_C(1302587248),UINT64_C(2593959001),UINT64_C(2420214292),UINT64_C(4255767389),UINT64_C(45229495),UINT64_C(1999311148),UINT64_C(264),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(2836398080),UINT64_C(867845914),UINT64_C(3478865213),UINT64_C(1689161839),UINT64_C(2307814704),UINT64_C(1901610038),UINT64_C(937348807),UINT64_C(9216548),UINT64_C(100373),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(1405157376),UINT64_C(3347442941),UINT64_C(462172198),UINT64_C(2464374426),UINT64_C(954029426),UINT64_C(624723905),UINT64_C(1442349023),UINT64_C(38094721),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(3579777024),UINT64_C(632727214),UINT64_C(2980814287),UINT64_C(3171342339),UINT64_C(4008807041),UINT64_C(2211478415),UINT64_C(1573246843),UINT64_C(3),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(2959155456),UINT64_C(687255411),UINT64_C(3248244966),UINT64_C(2074386530),UINT64_C(3801011509),UINT64_C(2650397687),UINT64_C(1277),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(828527888),UINT64_C(3393287680),UINT64_C(698399827),UINT64_C(667673988),UINT64_C(4291272133),UINT64_C(484895),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(4219425409),UINT64_C(2265142903),UINT64_C(4119564573),UINT64_C(1570454940),UINT64_C(184033331),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(268435456),UINT64_C(821090699),UINT64_C(2252078569),UINT64_C(771698833),UINT64_C(1126979233),UINT64_C(16),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(3774873600),UINT64_C(469881767),UINT64_C(649946844),UINT64_C(395721298),UINT64_C(6172),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(17825792),UINT64_C(2595180627),UINT64_C(3052466824),UINT64_C(2342503),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(3779133440),UINT64_C(497183526),UINT64_C(889054070),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(3870756864),UINT64_C(2416622419),UINT64_C(78),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(41853184),UINT64_C(29817),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(11316496),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(1),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
};

typedef struct {
  __m512i first;
  __m512i last;
} b58_mapped;

typedef struct {
  __m512i low;
  __m512i high;
} b58_raw;

static inline b58_mapped
b58_map_two_loads(const char *input, size_t input_len, int *valid) {
  const __m512i first = _mm512_loadu_si512((const void *)input);
  const __m512i last = _mm512_loadu_si512(
      (const void *)(input + input_len - 64));
  const __m512i table0 = _mm512_load_si512((const void *)B58_INV);
  const __m512i table1 = _mm512_load_si512((const void *)(B58_INV + 64));
  b58_mapped result;
  result.first = _mm512_permutex2var_epi8(table0, first, table1);
  result.last = _mm512_permutex2var_epi8(table0, last, table1);
  const __m512i bad_bytes = _mm512_or_si512(
      _mm512_or_si512(first, last),
      _mm512_or_si512(result.first, result.last));
  const __mmask64 bad = _mm512_movepi8_mask(bad_bytes);
  *valid = bad == 0;
  return result;
}

static inline size_t
b58_count_leading_ones(const char *input, size_t input_len) {
  if (input[0] != '1') return 0;
  const __m512i first = _mm512_loadu_si512((const void *)input);
  const uint64_t same = (uint64_t)_mm512_cmpeq_epi8_mask(
      first, _mm512_set1_epi8('1'));
  const uint64_t different = ~same;
  if (different) return (size_t)__builtin_ctzll(different);
  size_t leading = 64;
  while (leading < input_len && input[leading] == '1') ++leading;
  return leading;
}

static inline void
b58_frame_and_parse(b58_mapped mapped, unsigned pad, uint32_t cell[32]) {
  __m512i head;
  if (__builtin_expect(pad == 0, 1)) {
    head = mapped.first;
  } else if (__builtin_expect(pad == 1, 1)) {
    head = _mm512_maskz_permutexvar_epi8(
        (__mmask64)~UINT64_C(1),
        _mm512_load_si512((const void *)B58_HEAD_87), mapped.first);
  } else {
    const __m512i index = _mm512_sub_epi8(
        _mm512_load_si512((const void *)B58_IOTA),
        _mm512_set1_epi8((char)pad));
    const __mmask64 mask = (__mmask64)(UINT64_MAX << pad);
    head = _mm512_maskz_permutexvar_epi8(mask, index, mapped.first);
  }
  const __m512i tail = _mm512_maskz_permutexvar_epi8(
      (__mmask64)UINT64_C(0x00ffffff),
      _mm512_load_si512((const void *)B58_TAIL_24), mapped.last);
  const __m512i c2 = _mm512_set1_epi16(0x013a);      /* [58,1] */
  const __m512i c4 = _mm512_set1_epi32(0x00010d24); /* [3364,1] */
  const __m512i head_cell = _mm512_madd_epi16(
      _mm512_maddubs_epi16(head, c2), c4);
  const __m512i tail_cell = _mm512_madd_epi16(
      _mm512_maddubs_epi16(tail, c2), c4);
  _mm512_store_si512((void *)cell, head_cell);
  _mm512_store_si512((void *)(cell + 16), tail_cell);
}

#define ROW_ZZ(L, H, R) do {                                                \
  const __m512i d = _mm512_set1_epi32((int)cell[(R)]);                     \
  (L) = _mm512_add_epi64((L), _mm512_mul_epu32(                            \
      d, _mm512_load_si512((const void *)&B58_W32_B4[(R)][0])));           \
  (H) = _mm512_add_epi64((H), _mm512_mul_epu32(                            \
      d, _mm512_load_si512((const void *)&B58_W32_B4[(R)][8])));           \
} while (0)

#define ROW_ZY(L, H, R) do {                                                \
  const __m512i d = _mm512_set1_epi32((int)cell[(R)]);                     \
  (L) = _mm512_add_epi64((L), _mm512_mul_epu32(                            \
      d, _mm512_load_si512((const void *)&B58_W32_B4[(R)][0])));           \
  (H) = _mm256_add_epi64((H), _mm256_mul_epu32(                            \
      _mm512_castsi512_si256(d),                                           \
      _mm256_load_si256((const __m256i *)(const void *)                    \
                         &B58_W32_B4[(R)][8])));                            \
} while (0)

#define ROW_Z(L, R) do {                                                    \
  const __m512i d = _mm512_set1_epi32((int)cell[(R)]);                     \
  (L) = _mm512_add_epi64((L), _mm512_mul_epu32(                            \
      d, _mm512_load_si512((const void *)&B58_W32_B4[(R)][0])));           \
} while (0)

#define ROW_Y(L, R) do {                                                    \
  const __m256i d = _mm256_set1_epi32((int)cell[(R)]);                     \
  (L) = _mm256_add_epi64((L), _mm256_mul_epu32(                            \
      d, _mm256_load_si256((const __m256i *)(const void *)                 \
                            &B58_W32_B4[(R)][0])));                         \
} while (0)

/* Twenty-one ZMM products, ten YMM products, and one scalar tail multiply. */
static inline b58_raw
b58_matrix_full(const uint32_t cell[32]) {
  __m512i lo_a = _mm512_setzero_si512(), lo_b = lo_a;
  __m512i hi_a = lo_a, hi_b = lo_a;
  __m256i hy_a = _mm256_setzero_si256(), hy_b = hy_a;
  __m256i ly_a = hy_a, ly_b = hy_a;

  ROW_ZZ(lo_a, hi_a, 0); ROW_ZZ(lo_b, hi_b, 1);
  ROW_ZZ(lo_a, hi_a, 2); ROW_ZZ(lo_b, hi_b, 3);
  ROW_ZZ(lo_a, hi_a, 4);
  ROW_ZY(lo_b, hy_b, 5); ROW_ZY(lo_a, hy_a, 6);
  ROW_ZY(lo_b, hy_b, 7); ROW_ZY(lo_a, hy_a, 8);
  ROW_ZY(lo_b, hy_b, 9); ROW_ZY(lo_a, hy_a, 10);
  ROW_Z(lo_b, 11); ROW_Z(lo_a, 12); ROW_Z(lo_b, 13);
  ROW_Z(lo_a, 14); ROW_Z(lo_b, 15);
  ROW_Y(ly_a, 16); ROW_Y(ly_b, 17);
  ROW_Y(ly_a, 18); ROW_Y(ly_b, 19);

  __m512i low = _mm512_add_epi64(lo_a, lo_b);
  const __m256i low_tail = _mm256_add_epi64(ly_a, ly_b);
  low = _mm512_mask_add_epi64(
      low, (__mmask8)0x0f, low, _mm512_castsi256_si512(low_tail));
  const uint64_t scalar_tail =
      (uint64_t)cell[20] * (uint64_t)B58_B4 + (uint64_t)cell[21];
  low = _mm512_mask_add_epi64(
      low, (__mmask8)0x01, low, _mm512_set1_epi64((long long)scalar_tail));

  __m512i high = _mm512_add_epi64(hi_a, hi_b);
  const __m256i high_tail = _mm256_add_epi64(hy_a, hy_b);
  high = _mm512_mask_add_epi64(
      high, (__mmask8)0x0f, high, _mm512_castsi256_si512(high_tail));
  const b58_raw result = {low, high};
  return result;
}

#ifdef BRAID58_EXPERIMENT_ALL_ZMM
/* Experimental density control: spend full-width products on every nonempty
   half row.  Kept compile-time-only so the selected object has no dead body. */
static inline b58_raw
b58_matrix_full_all_zmm(const uint32_t cell[32]) {
  __m512i lo_a = _mm512_setzero_si512(), lo_b = lo_a;
  __m512i hi_a = lo_a, hi_b = lo_a;
  ROW_ZZ(lo_a, hi_a, 0); ROW_ZZ(lo_b, hi_b, 1);
  ROW_ZZ(lo_a, hi_a, 2); ROW_ZZ(lo_b, hi_b, 3);
  ROW_ZZ(lo_a, hi_a, 4); ROW_ZZ(lo_b, hi_b, 5);
  ROW_ZZ(lo_a, hi_a, 6); ROW_ZZ(lo_b, hi_b, 7);
  ROW_ZZ(lo_a, hi_a, 8); ROW_ZZ(lo_b, hi_b, 9);
  ROW_ZZ(lo_a, hi_a, 10);
  ROW_Z(lo_b, 11); ROW_Z(lo_a, 12); ROW_Z(lo_b, 13);
  ROW_Z(lo_a, 14); ROW_Z(lo_b, 15); ROW_Z(lo_a, 16);
  ROW_Z(lo_b, 17); ROW_Z(lo_a, 18); ROW_Z(lo_b, 19);
  ROW_Z(lo_a, 20); ROW_Z(lo_b, 21);
  const b58_raw result = {
    _mm512_add_epi64(lo_a, lo_b), _mm512_add_epi64(hi_a, hi_b)
  };
  return result;
}
#endif

/* The fallback skips every high B4 row known to be zero. */
static inline b58_raw
b58_matrix_skip(const uint32_t cell[32], unsigned first) {
  __m512i lo_a = _mm512_setzero_si512(), lo_b = lo_a;
  __m512i hi_a = lo_a, hi_b = lo_a;
  __m256i hy_a = _mm256_setzero_si256(), hy_b = hy_a;
  __m256i ly_a = hy_a, ly_b = hy_a;
  uint64_t scalar_tail = 0;
#define FALL __attribute__((fallthrough))
  switch (first) {
  case 0: ROW_ZZ(lo_a, hi_a, 0); FALL;
  case 1: ROW_ZZ(lo_b, hi_b, 1); FALL;
  case 2: ROW_ZZ(lo_a, hi_a, 2); FALL;
  case 3: ROW_ZZ(lo_b, hi_b, 3); FALL;
  case 4: ROW_ZZ(lo_a, hi_a, 4); FALL;
  case 5: ROW_ZY(lo_b, hy_b, 5); FALL;
  case 6: ROW_ZY(lo_a, hy_a, 6); FALL;
  case 7: ROW_ZY(lo_b, hy_b, 7); FALL;
  case 8: ROW_ZY(lo_a, hy_a, 8); FALL;
  case 9: ROW_ZY(lo_b, hy_b, 9); FALL;
  case 10: ROW_ZY(lo_a, hy_a, 10); FALL;
  case 11: ROW_Z(lo_b, 11); FALL;
  case 12: ROW_Z(lo_a, 12); FALL;
  case 13: ROW_Z(lo_b, 13); FALL;
  case 14: ROW_Z(lo_a, 14); FALL;
  case 15: ROW_Z(lo_b, 15); FALL;
  case 16: ROW_Y(ly_a, 16); FALL;
  case 17: ROW_Y(ly_b, 17); FALL;
  case 18: ROW_Y(ly_a, 18); FALL;
  case 19: ROW_Y(ly_b, 19); FALL;
  case 20: scalar_tail = (uint64_t)cell[20] * (uint64_t)B58_B4; FALL;
  case 21: scalar_tail += (uint64_t)cell[21]; break;
  default: break;
  }
#undef FALL
  __m512i low = _mm512_add_epi64(lo_a, lo_b);
  low = _mm512_mask_add_epi64(
      low, (__mmask8)0x0f, low,
      _mm512_castsi256_si512(_mm256_add_epi64(ly_a, ly_b)));
  low = _mm512_mask_add_epi64(
      low, (__mmask8)0x01, low, _mm512_set1_epi64((long long)scalar_tail));
  __m512i high = _mm512_add_epi64(hi_a, hi_b);
  high = _mm512_mask_add_epi64(
      high, (__mmask8)0x0f, high,
      _mm512_castsi256_si512(_mm256_add_epi64(hy_a, hy_b)));
  const b58_raw result = {low, high};
  return result;
}

#undef ROW_Y
#undef ROW_Z
#undef ROW_ZY
#undef ROW_ZZ

static inline int
b58_normalize_commit(b58_raw raw, size_t leading, uint8_t output[64]) {
  const __m512i mask32 = _mm512_set1_epi64((long long)UINT64_C(0xffffffff));
  const __m512i qlo = _mm512_srli_epi64(raw.low, 32);
  const __m512i qhi = _mm512_srli_epi64(raw.high, 32);
  const __m512i prev_lo = _mm512_maskz_permutexvar_epi64(
      (__mmask8)0xfe,
      _mm512_setr_epi64(0,0,1,2,3,4,5,6), qlo);
  const __m512i prev_hi = _mm512_permutex2var_epi64(
      qlo, _mm512_setr_epi64(7,8,9,10,11,12,13,14), qhi);
  const __m512i tlo = _mm512_add_epi64(
      _mm512_and_si512(raw.low, mask32), prev_lo);
  const __m512i thi = _mm512_add_epi64(
      _mm512_and_si512(raw.high, mask32), prev_hi);

  unsigned g = (unsigned)_mm512_cmp_epu64_mask(tlo, mask32, _MM_CMPINT_GT);
  g |= (unsigned)_mm512_cmp_epu64_mask(thi, mask32, _MM_CMPINT_GT) << 8;
  unsigned p = (unsigned)_mm512_cmpeq_epi64_mask(tlo, mask32);
  p |= (unsigned)_mm512_cmpeq_epi64_mask(thi, mask32) << 8;
  if (__builtin_expect(p != 0, 0)) {
    g |= p & (g << 1); p &= p << 1;
    g |= p & (g << 2); p &= p << 2;
    g |= p & (g << 4); p &= p << 4;
    g |= p & (g << 8);
  }
  g &= 0xffffU;
  if ((_mm512_cmpneq_epi64_mask(qhi, _mm512_setzero_si512()) & 0x80U) ||
      (g & 0x8000U)) return 0;

  const unsigned carry = (g << 1) & 0xffffU;
  const __m512i one = _mm512_set1_epi64(1);
  const __m512i ylo = _mm512_mask_add_epi64(
      tlo, (__mmask8)(carry & 0xffU), tlo, one);
  const __m512i yhi = _mm512_mask_add_epi64(
      thi, (__mmask8)(carry >> 8), thi, one);
  const __m256i wlo = _mm512_cvtepi64_epi32(ylo);
  const __m256i whi = _mm512_cvtepi64_epi32(yhi);
  __m512i words = _mm512_castsi256_si512(wlo);
  words = _mm512_inserti64x4(words, whi, 1);
  const __m512i be = _mm512_permutexvar_epi8(
      _mm512_load_si512((const void *)B58_REVERSE_64), words);

  if (__builtin_expect(leading == 0, 1)) {
    if (((uint32_t)_mm256_extract_epi32(whi, 7) &
         UINT32_C(0xff000000)) == 0) return 0;
  } else {
    const uint64_t nonzero = (uint64_t)_mm512_cmpneq_epi8_mask(
        be, _mm512_setzero_si512());
    const size_t zero_bytes = nonzero
        ? (size_t)__builtin_ctzll(nonzero) : 64U;
    if (leading != zero_bytes) return 0;
  }
  _mm512_storeu_si512((void *)output, be);
  return 1;
}

__attribute__((noinline, cold)) static int
b58_decode_fallback(const char *input, size_t input_len, uint8_t output[64]) {
  int valid;
  const b58_mapped mapped = b58_map_two_loads(input, input_len, &valid);
  if (!valid) return 0;
  const size_t leading = b58_count_leading_ones(input, input_len);
  _Alignas(64) uint32_t cell[32];
  const unsigned pad = (unsigned)(88U - input_len);
  b58_frame_and_parse(mapped, pad, cell);
  const unsigned first = (unsigned)((pad + leading) / 4U);
  return b58_normalize_commit(
      b58_matrix_skip(cell, first), leading, output);
}

int
braid58_decode_64_avx512(const char *input, size_t input_len,
                         uint8_t output[64]) {
  if (!input || !output || input_len < 64 || input_len > 88) return 0;
  if (__builtin_expect(input_len < 87 || input[0] == '1', 0))
    return b58_decode_fallback(input, input_len, output);

  int valid;
  const b58_mapped mapped = b58_map_two_loads(input, input_len, &valid);
  if (!valid) return 0;
  _Alignas(64) uint32_t cell[32];
  b58_frame_and_parse(mapped, (unsigned)(88U - input_len), cell);
#ifdef BRAID58_EXPERIMENT_ALL_ZMM
  return b58_normalize_commit(b58_matrix_full_all_zmm(cell), 0, output);
#else
  return b58_normalize_commit(b58_matrix_full(cell), 0, output);
#endif
}
