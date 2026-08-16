/*
 * AVX2 Bitcoin Base58 decoder for 64-byte outputs.
 *
 * B4 and B5 test bodies share the ASCII mapper and 16-column carry lookahead:
 *
 *   B4: 22 radix-58^4 cells.  Parsing is exactly vpmaddubsw+vpmaddwd;
 *       the matrix needs 54 four-column vpmuludq groups.
 *   B5: 18 radix-58^5 cells.  Parsing gathers the fifth digit separately;
 *       the matrix needs 44 four-column vpmuludq groups.
 *
 * The public entry point uses B4. Output is stored after alphabet, overflow,
 * width, and canonical-leading-zero validation.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

#define B58_B4 UINT32_C(11316496)

/* ASCII high-nibble tables.  0x80 marks an invalid low nibble. */
#define X UINT8_C(0x80)
_Alignas(32) static const uint8_t B58_MAP_3[32] = {
    X,0,1,2,3,4,5,6,7,8,X,X,X,X,X,X,
    X,0,1,2,3,4,5,6,7,8,X,X,X,X,X,X};
_Alignas(32) static const uint8_t B58_MAP_4[32] = {
    X,9,10,11,12,13,14,15,16,X,17,18,19,20,21,X,
    X,9,10,11,12,13,14,15,16,X,17,18,19,20,21,X};
_Alignas(32) static const uint8_t B58_MAP_5[32] = {
    22,23,24,25,26,27,28,29,30,31,32,X,X,X,X,X,
    22,23,24,25,26,27,28,29,30,31,32,X,X,X,X,X};
_Alignas(32) static const uint8_t B58_MAP_6[32] = {
    X,33,34,35,36,37,38,39,40,41,42,43,X,44,45,46,
    X,33,34,35,36,37,38,39,40,41,42,43,X,44,45,46};
_Alignas(32) static const uint8_t B58_MAP_7[32] = {
    47,48,49,50,51,52,53,54,55,56,57,X,X,X,X,X,
    47,48,49,50,51,52,53,54,55,56,57,X,X,X,X,X};
#undef X

_Alignas(64) static const uint32_t B58_EXPAND4[16] = {
    0x00000000,0x00000001,0x00000100,0x00000101,
    0x00010000,0x00010001,0x00010100,0x00010101,
    0x01000000,0x01000001,0x01000100,0x01000101,
    0x01010000,0x01010001,0x01010100,0x01010101,
};

/* B5 row i is (58^5)^(17-i), little-endian in radix 2^32. */
#ifdef BRAID58_BENCH_VARIANTS
_Alignas(32) static const uint64_t B58_W32_B5[18][16] = {
  {UINT64_C(0),UINT64_C(0),UINT64_C(1570766848),UINT64_C(1540739182),UINT64_C(3863405776),UINT64_C(2901437456),UINT64_C(3728167192),UINT64_C(3840888383),UINT64_C(627529458),UINT64_C(1035889824),UINT64_C(2498525019),UINT64_C(3115810883),UINT64_C(4021557284),UINT64_C(173911550),UINT64_C(3719864065),UINT64_C(249448)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(104923136),UINT64_C(669472826),UINT64_C(2597060712),UINT64_C(1695615427),UINT64_C(3960476767),UINT64_C(3577221846),UINT64_C(1062993857),UINT64_C(2005415586),UINT64_C(2618421812),UINT64_C(1023671068),UINT64_C(4128706713),UINT64_C(1882780341),UINT64_C(1632305),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(1103833088),UINT64_C(2290070198),UINT64_C(713340517),UINT64_C(997594232),UINT64_C(2549553452),UINT64_C(2414104418),UINT64_C(4169135587),UINT64_C(2143913881),UINT64_C(4058671871),UINT64_C(2406345166),UINT64_C(1422956801),UINT64_C(10681231),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(910779968),UINT64_C(1699516363),UINT64_C(3410618104),UINT64_C(2761740102),UINT64_C(314610629),UINT64_C(3492037905),UINT64_C(2301468615),UINT64_C(1285619000),UINT64_C(1785020643),UINT64_C(1038812943),UINT64_C(69894212),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(0),UINT64_C(2579227194),UINT64_C(439156799),UINT64_C(2088408376),UINT64_C(1828091393),UINT64_C(3716679421),UINT64_C(2107865525),UINT64_C(1389513021),UINT64_C(3976106370),UINT64_C(927569770),UINT64_C(457363084),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(268435456),UINT64_C(2209946163),UINT64_C(3495303162),UINT64_C(486575164),UINT64_C(1447250220),UINT64_C(339767049),UINT64_C(112778334),UINT64_C(3862831115),UINT64_C(383623235),UINT64_C(2992822783),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(176160768),UINT64_C(2361423716),UINT64_C(307953032),UINT64_C(1429430446),UINT64_C(2266258239),UINT64_C(1893006839),UINT64_C(3998086794),UINT64_C(2962826229),UINT64_C(2404108010),UINT64_C(4),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(2485387264),UINT64_C(3688771808),UINT64_C(4264082837),UINT64_C(868688145),UINT64_C(1014420882),UINT64_C(1332209423),UINT64_C(3044036677),UINT64_C(3596590989),UINT64_C(29),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(1468637184),UINT64_C(2338440092),UINT64_C(1088536814),UINT64_C(3549229270),UINT64_C(582574436),UINT64_C(3711696540),UINT64_C(1054003707),UINT64_C(195),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(2959155456),UINT64_C(687255411),UINT64_C(3248244966),UINT64_C(2074386530),UINT64_C(3801011509),UINT64_C(2650397687),UINT64_C(1277),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(0),UINT64_C(1199103528),UINT64_C(132556120),UINT64_C(3418394749),UINT64_C(3047609191),UINT64_C(1184754854),UINT64_C(8360),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(1073741824),UINT64_C(485140318),UINT64_C(3964963911),UINT64_C(1834629191),UINT64_C(2996985344),UINT64_C(54706),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(4194304000),UINT64_C(1483338760),UINT64_C(3337178590),UINT64_C(1476998812),UINT64_C(357981),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(17825792),UINT64_C(2595180627),UINT64_C(3052466824),UINT64_C(2342503),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(4063920128),UINT64_C(1933902296),UINT64_C(15328518),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(3355157504),UINT64_C(100304420),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(656356768),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)},
  {UINT64_C(1),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)}
};
#endif

/* B4 row i is (58^4)^(21-i), little-endian in radix 2^32. */
_Alignas(32) static const uint64_t B58_W32_B4[22][16] = {
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
  {UINT64_C(1),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0),UINT64_C(0)}
};

static inline __m256i b58_map_ascii(__m256i ch) {
  const __m256i nibble = _mm256_set1_epi8(15);
  const __m256i lo = _mm256_and_si256(ch, nibble);
  const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(ch, 4), nibble);
  const __m256i u = _mm256_sub_epi8(hi, _mm256_set1_epi8(3));
  const __m256i m0 = _mm256_slli_epi16(u, 7);
  const __m256i m1 = _mm256_slli_epi16(u, 6);
  const __m256i m2 = _mm256_slli_epi16(u, 5);
  const __m256i t3 = _mm256_shuffle_epi8(
      _mm256_load_si256((const __m256i *)(const void *)B58_MAP_3), lo);
  const __m256i t4 = _mm256_shuffle_epi8(
      _mm256_load_si256((const __m256i *)(const void *)B58_MAP_4), lo);
  const __m256i t5 = _mm256_shuffle_epi8(
      _mm256_load_si256((const __m256i *)(const void *)B58_MAP_5), lo);
  const __m256i t6 = _mm256_shuffle_epi8(
      _mm256_load_si256((const __m256i *)(const void *)B58_MAP_6), lo);
  const __m256i t7 = _mm256_shuffle_epi8(
      _mm256_load_si256((const __m256i *)(const void *)B58_MAP_7), lo);
  const __m256i a = _mm256_blendv_epi8(t3, t4, m0);
  const __m256i b = _mm256_blendv_epi8(t5, t6, m0);
  __m256i value = _mm256_blendv_epi8(a, b, m1);
  value = _mm256_blendv_epi8(value, t7, m2);
  const __m256i below = _mm256_cmpgt_epi8(_mm256_set1_epi8(3), hi);
  const __m256i above = _mm256_cmpgt_epi8(hi, _mm256_set1_epi8(7));
  return _mm256_or_si256(value, _mm256_or_si256(below, above));
}

/* Maps input into a right-aligned numeric digit vector and returns the count
   of literal leading '1' characters.  input_len>=64 guarantees all loads. */
static inline size_t b58_map_input(const char *input, size_t input_len,
                                   uint8_t *digit, size_t total_digits,
                                   int *valid) {
  const __m256i ch0 = _mm256_loadu_si256((const __m256i *)(const void *)input);
  const __m256i ch1 = _mm256_loadu_si256(
      (const __m256i *)(const void *)(input + 32));
  const __m256i ch2 = _mm256_loadu_si256(
      (const __m256i *)(const void *)(input + input_len - 32));
  const __m256i d0 = b58_map_ascii(ch0);
  const __m256i d1 = b58_map_ascii(ch1);
  const __m256i d2 = b58_map_ascii(ch2);
  *valid = _mm256_movemask_epi8(
      _mm256_or_si256(d0, _mm256_or_si256(d1, d2))) == 0;
  const size_t pad = total_digits - input_len;
  _mm256_storeu_si256((__m256i *)(void *)(digit + pad), d0);
  _mm256_storeu_si256((__m256i *)(void *)(digit + pad + 32), d1);
  _mm256_storeu_si256((__m256i *)(void *)(digit + total_digits - 32), d2);

  size_t leading = 0;
  if (__builtin_expect(input[0] == '1', 0)) {
    const __m256i one = _mm256_set1_epi8('1');
    uint32_t different = ~(uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(ch0, one));
    if (different) {
      leading = (size_t)__builtin_ctz(different);
    } else {
      different = ~(uint32_t)_mm256_movemask_epi8(
          _mm256_cmpeq_epi8(ch1, one));
      if (different) {
        leading = 32U + (size_t)__builtin_ctz(different);
      } else {
        leading = 64U;
        while (leading < input_len && input[leading] == '1')
          ++leading;
      }
    }
  }
  return leading;
}

static inline void b58_store_cells(__m256i cell, uint32_t *dst) {
  _mm256_store_si256((__m256i *)(void *)dst, cell);
}

static inline void b58_parse_b4(const uint8_t *digit, uint32_t cell[24]) {
  const __m256i c2 = _mm256_set1_epi16(0x013a);       /* [58,1] */
  const __m256i c4 = _mm256_set1_epi32(0x00010d24);  /* [3364,1] */
  for (unsigned i = 0; i < 3; ++i) {
    const __m256i d = _mm256_load_si256(
        (const __m256i *)(const void *)(digit + 32U * i));
    const __m256i pair = _mm256_maddubs_epi16(d, c2);
    b58_store_cells(_mm256_madd_epi16(pair, c4), cell + 8U * i);
  }
}

#ifdef BRAID58_BENCH_VARIANTS
_Alignas(32) static const uint8_t B58_B5_FIRST_A[32] = {
  0,1,2,3,5,6,7,8,10,11,12,13,15,0x80,0x80,0x80,
  0,1,2,3,5,6,7,8,10,11,12,13,15,0x80,0x80,0x80};
_Alignas(32) static const uint8_t B58_B5_FIRST_B[32] = {
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
  0x80,0x80,0x80,0x80,0x80,0,1,2,
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
  0x80,0x80,0x80,0x80,0x80,0,1,2};
_Alignas(32) static const uint8_t B58_B5_LAST_A[32] = {
  4,0x80,0x80,0x80,9,0x80,0x80,0x80,
  14,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
  4,0x80,0x80,0x80,9,0x80,0x80,0x80,
  14,0x80,0x80,0x80,0x80,0x80,0x80,0x80};
_Alignas(32) static const uint8_t B58_B5_LAST_B[32] = {
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
  0x80,0x80,0x80,0x80,3,0x80,0x80,0x80,
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
  0x80,0x80,0x80,0x80,3,0x80,0x80,0x80};

static inline void b58_parse_b5(const uint8_t *digit, uint32_t cell[24]) {
  const __m256i c2 = _mm256_set1_epi16(0x013a);
  const __m256i c4 = _mm256_set1_epi32(0x00010d24);
  const __m256i c58 = _mm256_set1_epi32(58);
  const __m256i ma = _mm256_load_si256(
      (const __m256i *)(const void *)B58_B5_FIRST_A);
  const __m256i mb = _mm256_load_si256(
      (const __m256i *)(const void *)B58_B5_FIRST_B);
  const __m256i la = _mm256_load_si256(
      (const __m256i *)(const void *)B58_B5_LAST_A);
  const __m256i lb = _mm256_load_si256(
      (const __m256i *)(const void *)B58_B5_LAST_B);
  for (unsigned block = 0; block < 3; ++block) {
    const uint8_t *p = digit + 40U * block;
    __m256i a = _mm256_castsi128_si256(
        _mm_loadu_si128((const __m128i *)(const void *)p));
    a = _mm256_inserti128_si256(a,
        _mm_loadu_si128((const __m128i *)(const void *)(p + 20)), 1);
    __m256i b = _mm256_castsi128_si256(
        _mm_loadu_si128((const __m128i *)(const void *)(p + 16)));
    b = _mm256_inserti128_si256(b,
        _mm_loadu_si128((const __m128i *)(const void *)(p + 36)), 1);
    const __m256i first = _mm256_or_si256(
        _mm256_shuffle_epi8(a, ma), _mm256_shuffle_epi8(b, mb));
    const __m256i last = _mm256_or_si256(
        _mm256_shuffle_epi8(a, la), _mm256_shuffle_epi8(b, lb));
    const __m256i pair = _mm256_maddubs_epi16(first, c2);
    const __m256i four = _mm256_madd_epi16(pair, c4);
    b58_store_cells(_mm256_add_epi32(_mm256_mullo_epi32(four, c58), last),
                    cell + 8U * block);
  }
}
#endif

static inline __m256i b58_broadcast_cell(const uint32_t *cell) {
  return _mm256_set1_epi32((int)*cell);
}

/* GCC otherwise keeps dozens of independent products live while scheduling
   the unrolled matrix and spills most of them to a ~1.2 KiB frame.  Keep each
   product adjacent to its accumulation in one fixed scratch register. */
static inline __m256i b58_mac(__m256i acc, __m256i digit,
                              const __m256i *weight) {
  __asm__ volatile(
      "vpmuludq %2,%1,%%ymm15\n\t"
      "vpaddq %%ymm15,%0,%0"
      : "+x"(acc)
      : "x"(digit), "m"(*weight)
      : "ymm15");
  return acc;
}

#define MULADD(A, D, W, O) \
  (A) = b58_mac((A), (D), \
      (const __m256i *)(const void *)&(W)[(O)])

#define ROW4(P, W, R) do { const __m256i d=b58_broadcast_cell(&cell[(R)]); \
  MULADD(P##0,d,(W)[(R)],0); MULADD(P##1,d,(W)[(R)],4); \
  MULADD(P##2,d,(W)[(R)],8); MULADD(P##3,d,(W)[(R)],12); } while(0)
#define ROW3(P, W, R) do { const __m256i d=b58_broadcast_cell(&cell[(R)]); \
  MULADD(P##0,d,(W)[(R)],0); MULADD(P##1,d,(W)[(R)],4); \
  MULADD(P##2,d,(W)[(R)],8); } while(0)
#define ROW2(P, W, R) do { const __m256i d=b58_broadcast_cell(&cell[(R)]); \
  MULADD(P##0,d,(W)[(R)],0); MULADD(P##1,d,(W)[(R)],4); } while(0)
#define ROW1(P, W, R) do { const __m256i d=b58_broadcast_cell(&cell[(R)]); \
  MULADD(P##0,d,(W)[(R)],0); } while(0)

#ifdef BRAID58_BENCH_VARIANTS
static inline void b58_matrix_b5(const uint32_t cell[24], __m256i raw[4]) {
  __m256i a0=_mm256_setzero_si256(),a1=a0,a2=a0,a3=a0;
  __m256i b0=a0,b1=a0,b2=a0,b3=a0;
  ROW4(a,B58_W32_B5,0); ROW4(b,B58_W32_B5,1);
  ROW4(a,B58_W32_B5,2); ROW4(b,B58_W32_B5,3);
  ROW3(a,B58_W32_B5,4); ROW3(b,B58_W32_B5,5);
  ROW3(a,B58_W32_B5,6); ROW3(b,B58_W32_B5,7);
  ROW3(a,B58_W32_B5,8);
  ROW2(b,B58_W32_B5,9); ROW2(a,B58_W32_B5,10);
  ROW2(b,B58_W32_B5,11); ROW2(a,B58_W32_B5,12);
  ROW1(b,B58_W32_B5,13); ROW1(a,B58_W32_B5,14);
  ROW1(b,B58_W32_B5,15); ROW1(a,B58_W32_B5,16);
  ROW1(b,B58_W32_B5,17);
  raw[0]=_mm256_add_epi64(a0,b0); raw[1]=_mm256_add_epi64(a1,b1);
  raw[2]=_mm256_add_epi64(a2,b2); raw[3]=_mm256_add_epi64(a3,b3);
}
#endif

static inline void b58_matrix_b4(const uint32_t cell[24], __m256i raw[4]) {
  __m256i a0=_mm256_setzero_si256(),a1=a0,a2=a0,a3=a0;
  __m256i b0=a0,b1=a0,b2=a0,b3=a0;
  ROW4(a,B58_W32_B4,0); ROW4(b,B58_W32_B4,1);
  ROW4(a,B58_W32_B4,2); ROW4(b,B58_W32_B4,3);
  ROW4(a,B58_W32_B4,4); ROW3(b,B58_W32_B4,5);
  ROW3(a,B58_W32_B4,6); ROW3(b,B58_W32_B4,7);
  ROW3(a,B58_W32_B4,8); ROW3(b,B58_W32_B4,9);
  ROW3(a,B58_W32_B4,10);
  ROW2(b,B58_W32_B4,11); ROW2(a,B58_W32_B4,12);
  ROW2(b,B58_W32_B4,13); ROW2(a,B58_W32_B4,14);
  ROW2(b,B58_W32_B4,15); ROW1(a,B58_W32_B4,16);
  ROW1(b,B58_W32_B4,17); ROW1(a,B58_W32_B4,18);
  ROW1(b,B58_W32_B4,19); ROW1(a,B58_W32_B4,20);
  ROW1(b,B58_W32_B4,21);
  raw[0]=_mm256_add_epi64(a0,b0); raw[1]=_mm256_add_epi64(a1,b1);
  raw[2]=_mm256_add_epi64(a2,b2); raw[3]=_mm256_add_epi64(a3,b3);
}

/* The fixed-width representation is left-padded.  Literal leading '1' digits
   therefore make whole high B4 rows provably zero.  The all-active case above
   stays branch-free; this fall-through schedule is only for inputs with at
   least one complete zero cell. */
__attribute__((always_inline)) static inline void
b58_matrix_b4_skip(const uint32_t cell[24], unsigned first, __m256i raw[4]) {
  __m256i a0=_mm256_setzero_si256(),a1=a0,a2=a0,a3=a0;
  __m256i b0=a0,b1=a0,b2=a0,b3=a0;
#define FALL __attribute__((fallthrough))
  switch (first) {
  case 0:  ROW4(a,B58_W32_B4,0);  FALL;
  case 1:  ROW4(b,B58_W32_B4,1);  FALL;
  case 2:  ROW4(a,B58_W32_B4,2);  FALL;
  case 3:  ROW4(b,B58_W32_B4,3);  FALL;
  case 4:  ROW4(a,B58_W32_B4,4);  FALL;
  case 5:  ROW3(b,B58_W32_B4,5);  FALL;
  case 6:  ROW3(a,B58_W32_B4,6);  FALL;
  case 7:  ROW3(b,B58_W32_B4,7);  FALL;
  case 8:  ROW3(a,B58_W32_B4,8);  FALL;
  case 9:  ROW3(b,B58_W32_B4,9);  FALL;
  case 10: ROW3(a,B58_W32_B4,10); FALL;
  case 11: ROW2(b,B58_W32_B4,11); FALL;
  case 12: ROW2(a,B58_W32_B4,12); FALL;
  case 13: ROW2(b,B58_W32_B4,13); FALL;
  case 14: ROW2(a,B58_W32_B4,14); FALL;
  case 15: ROW2(b,B58_W32_B4,15); FALL;
  case 16: ROW1(a,B58_W32_B4,16); FALL;
  case 17: ROW1(b,B58_W32_B4,17); FALL;
  case 18: ROW1(a,B58_W32_B4,18); FALL;
  case 19: ROW1(b,B58_W32_B4,19); FALL;
  case 20: ROW1(a,B58_W32_B4,20); FALL;
  case 21: ROW1(b,B58_W32_B4,21); break;
  default: break;
  }
#undef FALL
  raw[0]=_mm256_add_epi64(a0,b0); raw[1]=_mm256_add_epi64(a1,b1);
  raw[2]=_mm256_add_epi64(a2,b2); raw[3]=_mm256_add_epi64(a3,b3);
}

#undef ROW1
#undef ROW2
#undef ROW3
#undef ROW4
#undef MULADD

static inline __m256i b58_shift_q(__m256i cur, __m256i next) {
  const __m256i cross = _mm256_permute2x128_si256(cur, next, 0x21);
  return _mm256_alignr_epi8(next, cross, 8);
}

static inline __m256i b58_carry4(unsigned bits) {
  return _mm256_cvtepu8_epi64(
      _mm_cvtsi32_si128((int)B58_EXPAND4[bits & 15U]));
}

static inline int b58_commit_words(__m256i w0, __m256i w1,
                                   size_t leading_ones,
                                   uint8_t output[64]) {
  const __m256i rev=_mm256_setr_epi32(7,6,5,4,3,2,1,0);
  const __m256i bswap=_mm256_setr_epi8(
      3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,
      3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
  const __m256i be0=_mm256_shuffle_epi8(
      _mm256_permutevar8x32_epi32(w1,rev),bswap);
  const __m256i be1=_mm256_shuffle_epi8(
      _mm256_permutevar8x32_epi32(w0,rev),bswap);
  if (__builtin_expect(leading_ones==0,1)) {
    /* A canonical full-width value must have a nonzero most-significant byte.
       This avoids two stack stores and a 16-limb scan on the common path. */
    if (((uint32_t)_mm256_extract_epi32(w1,7)&UINT32_C(0xff000000))==0)
      return 0;
  } else {
    const __m256i z=_mm256_setzero_si256();
    const uint32_t nz0=~(uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(be0,z));
    size_t leading_zero_bytes;
    if (nz0) {
      leading_zero_bytes=(size_t)__builtin_ctz(nz0);
    } else {
      const uint32_t nz1=~(uint32_t)_mm256_movemask_epi8(
          _mm256_cmpeq_epi8(be1,z));
      leading_zero_bytes=nz1 ? 32U+(size_t)__builtin_ctz(nz1) : 64U;
    }
    if (leading_ones!=leading_zero_bytes)
      return 0;
  }
  _mm256_storeu_si256((__m256i *)(void *)output,be0);
  _mm256_storeu_si256((__m256i *)(void *)(output+32),be1);
  return 1;
}

static inline int b58_normalize_commit(const __m256i raw[4],
                                       size_t leading_ones,
                                       uint8_t output[64]) {
  const __m256i mask32 = _mm256_set1_epi64x((long long)UINT64_C(0xffffffff));
  const __m256i q0=_mm256_srli_epi64(raw[0],32);
  const __m256i q1=_mm256_srli_epi64(raw[1],32);
  const __m256i q2=_mm256_srli_epi64(raw[2],32);
  const __m256i q3=_mm256_srli_epi64(raw[3],32);
  __m256i p0=_mm256_permute4x64_epi64(q0,0x90);
  p0=_mm256_blend_epi32(_mm256_setzero_si256(),p0,0xfc);
  const __m256i t0=_mm256_add_epi64(_mm256_and_si256(raw[0],mask32),p0);
  const __m256i t1=_mm256_add_epi64(_mm256_and_si256(raw[1],mask32),b58_shift_q(q0,q1));
  const __m256i t2=_mm256_add_epi64(_mm256_and_si256(raw[2],mask32),b58_shift_q(q1,q2));
  const __m256i t3=_mm256_add_epi64(_mm256_and_si256(raw[3],mask32),b58_shift_q(q2,q3));
  unsigned g=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(t0,mask32)));
  g|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(t1,mask32)))<<4;
  g|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(t2,mask32)))<<8;
  g|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(t3,mask32)))<<12;
  unsigned p=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(t0,mask32)));
  p|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(t1,mask32)))<<4;
  p|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(t2,mask32)))<<8;
  p|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(t3,mask32)))<<12;
  if (__builtin_expect(p!=0,0)) {
    g|=p&(g<<1); p&=p<<1;
    g|=p&(g<<2); p&=p<<2;
    g|=p&(g<<4); p&=p<<4;
    g|=p&(g<<8);
  }
  g&=0xffffU;
  if ((uint64_t)_mm256_extract_epi64(q3,3)!=0 || (g&0x8000U)!=0)
    return 0;
  const unsigned ci=(g<<1)&0xffffU;
  const __m256i y0=_mm256_add_epi64(t0,b58_carry4(ci));
  const __m256i y1=_mm256_add_epi64(t1,b58_carry4(ci>>4));
  const __m256i y2=_mm256_add_epi64(t2,b58_carry4(ci>>8));
  const __m256i y3=_mm256_add_epi64(t3,b58_carry4(ci>>12));
  const __m256i ix=_mm256_setr_epi32(0,2,4,6,0,0,0,0);
  const __m256i w0=_mm256_permute2x128_si256(
      _mm256_permutevar8x32_epi32(y0,ix),_mm256_permutevar8x32_epi32(y1,ix),0x20);
  const __m256i w1=_mm256_permute2x128_si256(
      _mm256_permutevar8x32_epi32(y2,ix),_mm256_permutevar8x32_epi32(y3,ix),0x20);
  return b58_commit_words(w0,w1,leading_ones,output);
}

/* Experimental A/B baseline: safe for B4's <=59-bit raw columns.  Kept out
   of the public path so the Boolean carry-lookahead can be measured against a
   conventional sixteen-link scalar dependency chain. */
#ifdef BRAID58_BENCH_VARIANTS
static inline int b58_normalize_commit_serial_b4(const __m256i raw[4],
                                                 size_t leading_ones,
                                                 uint8_t output[64]) {
  _Alignas(32) uint64_t col[16];
  _Alignas(32) uint32_t word[16];
  for (unsigned v=0;v<4;++v)
    _mm256_store_si256((__m256i *)(void *)(col+4U*v),raw[v]);
  uint64_t carry=0;
  for (unsigned i=0;i<16;++i) {
    const uint64_t value=col[i]+carry;
    word[i]=(uint32_t)value;
    carry=value>>32;
  }
  if (carry) return 0;
  return b58_commit_words(
      _mm256_load_si256((const __m256i *)(const void *)word),
      _mm256_load_si256((const __m256i *)(const void *)(word+8)),
      leading_ones,output);
}
#endif

/* Experimental two-input schedule.  One accumulator per output tile and per
   input gives each chain eight independent MACs of distance.  Each weight is
   loaded once, then used for both inputs. */
#define X2MAC(A, B, DA, DB, W, O) do {                                    \
  const __m256i wt = _mm256_load_si256(                                   \
      (const __m256i *)(const void *)&(W)[(O)]);                           \
  __asm__ volatile(                                                        \
      "vpmuludq %4,%2,%%ymm14\n\t"                                       \
      "vpmuludq %4,%3,%%ymm15\n\t"                                       \
      "vpaddq %%ymm14,%0,%0\n\t"                                         \
      "vpaddq %%ymm15,%1,%1"                                             \
      : "+x"(A), "+x"(B)                                                \
      : "x"(DA), "x"(DB), "x"(wt)                                      \
      : "ymm14", "ymm15");                                             \
} while (0)

#define X2ROW4(W, R) do {                                                   \
  const __m256i da=b58_broadcast_cell(&cell0[(R)]);                         \
  const __m256i db=b58_broadcast_cell(&cell1[(R)]);                         \
  X2MAC(a0,b0,da,db,(W)[(R)],0); X2MAC(a1,b1,da,db,(W)[(R)],4);            \
  X2MAC(a2,b2,da,db,(W)[(R)],8); X2MAC(a3,b3,da,db,(W)[(R)],12);           \
} while (0)
#define X2ROW3(W, R) do {                                                   \
  const __m256i da=b58_broadcast_cell(&cell0[(R)]);                         \
  const __m256i db=b58_broadcast_cell(&cell1[(R)]);                         \
  X2MAC(a0,b0,da,db,(W)[(R)],0); X2MAC(a1,b1,da,db,(W)[(R)],4);            \
  X2MAC(a2,b2,da,db,(W)[(R)],8);                                           \
} while (0)
#define X2ROW2(W, R) do {                                                   \
  const __m256i da=b58_broadcast_cell(&cell0[(R)]);                         \
  const __m256i db=b58_broadcast_cell(&cell1[(R)]);                         \
  X2MAC(a0,b0,da,db,(W)[(R)],0); X2MAC(a1,b1,da,db,(W)[(R)],4);            \
} while (0)
#define X2ROW1(W, R) do {                                                   \
  const __m256i da=b58_broadcast_cell(&cell0[(R)]);                         \
  const __m256i db=b58_broadcast_cell(&cell1[(R)]);                         \
  X2MAC(a0,b0,da,db,(W)[(R)],0);                                           \
} while (0)

static inline void b58_matrix_b4_x2(const uint32_t cell0[24],
                                     const uint32_t cell1[24],
                                     __m256i raw0[4], __m256i raw1[4]) {
  __m256i a0=_mm256_setzero_si256(),a1=a0,a2=a0,a3=a0;
  __m256i b0=a0,b1=a0,b2=a0,b3=a0;
  X2ROW4(B58_W32_B4,0); X2ROW4(B58_W32_B4,1);
  X2ROW4(B58_W32_B4,2); X2ROW4(B58_W32_B4,3);
  X2ROW4(B58_W32_B4,4); X2ROW3(B58_W32_B4,5);
  X2ROW3(B58_W32_B4,6); X2ROW3(B58_W32_B4,7);
  X2ROW3(B58_W32_B4,8); X2ROW3(B58_W32_B4,9);
  X2ROW3(B58_W32_B4,10);
  X2ROW2(B58_W32_B4,11); X2ROW2(B58_W32_B4,12);
  X2ROW2(B58_W32_B4,13); X2ROW2(B58_W32_B4,14);
  X2ROW2(B58_W32_B4,15); X2ROW1(B58_W32_B4,16);
  X2ROW1(B58_W32_B4,17); X2ROW1(B58_W32_B4,18);
  X2ROW1(B58_W32_B4,19); X2ROW1(B58_W32_B4,20);
  X2ROW1(B58_W32_B4,21);
  raw0[0]=a0; raw0[1]=a1; raw0[2]=a2; raw0[3]=a3;
  raw1[0]=b0; raw1[1]=b1; raw1[2]=b2; raw1[3]=b3;
}

#undef X2ROW1
#undef X2ROW2
#undef X2ROW3
#undef X2ROW4
#undef X2MAC

static inline int b58_decode_64_b4_core(const char *input,size_t input_len,
                                        uint8_t output[64]) {
  if (!input || !output || input_len<64 || input_len>88) return 0;
  _Alignas(32) uint8_t digit[96];
  const __m256i z=_mm256_setzero_si256();
  _mm256_store_si256((__m256i *)(void *)(digit+0),z);
  _mm256_store_si256((__m256i *)(void *)(digit+32),z);
  _mm256_store_si256((__m256i *)(void *)(digit+64),z);
  int valid;
  const size_t leading=b58_map_input(input,input_len,digit,88,&valid);
  if (!valid) return 0;
  _Alignas(32) uint32_t cell[24];
  b58_parse_b4(digit,cell);
  __m256i raw[4];
  const unsigned zero_cells=(unsigned)((88U-input_len+leading)/4U);
  if (__builtin_expect(zero_cells==0,1))
    b58_matrix_b4(cell,raw);
  else
    b58_matrix_b4_skip(cell,zero_cells,raw);
  return b58_normalize_commit(raw,leading,output);
}

__attribute__((noinline,cold)) static unsigned
b58_decode_64x2_fallback(const char *input0,size_t input_len0,
                          uint8_t output0[64],const char *input1,
                          size_t input_len1,uint8_t output1[64]) {
  unsigned mask=0;
  if (b58_decode_64_b4_core(input0,input_len0,output0)) mask|=1U;
  if (b58_decode_64_b4_core(input1,input_len1,output1)) mask|=2U;
  return mask;
}

/* Batched entry point.  Bits 0 and 1 report independent lane success.  Each
   failed lane's output remains untouched (outputs must not overlap).  Bad
   pointers/lengths/alphabet bytes use a cold independent-lane fallback so the
   valid two-lane schedule retains its compact register allocation. */
unsigned braid58_decode_64x2_avx2(const char *input0,size_t input_len0,
                                  uint8_t output0[64],const char *input1,
                                  size_t input_len1,uint8_t output1[64]) {
  if (__builtin_expect(!input0 || !input1 || !output0 || !output1 ||
      input_len0<64 || input_len0>88 || input_len1<64 || input_len1>88,0))
    return b58_decode_64x2_fallback(input0,input_len0,output0,
                                    input1,input_len1,output1);
  _Alignas(32) uint8_t digit0[96],digit1[96];
  const __m256i z=_mm256_setzero_si256();
  for (unsigned i=0;i<3;++i) {
    _mm256_store_si256((__m256i *)(void *)(digit0+32U*i),z);
    _mm256_store_si256((__m256i *)(void *)(digit1+32U*i),z);
  }
  int valid0,valid1;
  const size_t leading0=b58_map_input(input0,input_len0,digit0,88,&valid0);
  const size_t leading1=b58_map_input(input1,input_len1,digit1,88,&valid1);
  if (__builtin_expect(!valid0 || !valid1,0))
    return b58_decode_64x2_fallback(input0,input_len0,output0,
                                    input1,input_len1,output1);
  _Alignas(32) uint32_t cell0[24],cell1[24];
  b58_parse_b4(digit0,cell0);
  b58_parse_b4(digit1,cell1);
  __m256i raw0[4],raw1[4];
  b58_matrix_b4_x2(cell0,cell1,raw0,raw1);
  unsigned mask=0;
  if (b58_normalize_commit(raw0,leading0,output0)) mask|=1U;
  if (b58_normalize_commit(raw1,leading1,output1)) mask|=2U;
  return mask;
}

#ifdef BRAID58_BENCH_VARIANTS
/* Exposed only to the in-tree A/B harness. */
int braid58_decode_64_avx2_b4(const char *input,size_t input_len,
                              uint8_t output[64]) {
  return b58_decode_64_b4_core(input,input_len,output);
}

int braid58_decode_64_avx2_b4_serial(const char *input,size_t input_len,
                                     uint8_t output[64]) {
  if (!input || !output || input_len<64 || input_len>88) return 0;
  _Alignas(32) uint8_t digit[96];
  const __m256i z=_mm256_setzero_si256();
  _mm256_store_si256((__m256i *)(void *)(digit+0),z);
  _mm256_store_si256((__m256i *)(void *)(digit+32),z);
  _mm256_store_si256((__m256i *)(void *)(digit+64),z);
  int valid;
  const size_t leading=b58_map_input(input,input_len,digit,88,&valid);
  if (!valid) return 0;
  _Alignas(32) uint32_t cell[24];
  b58_parse_b4(digit,cell);
  __m256i raw[4];
  const unsigned zero_cells=(unsigned)((88U-input_len+leading)/4U);
  if (__builtin_expect(zero_cells==0,1))
    b58_matrix_b4(cell,raw);
  else
    b58_matrix_b4_skip(cell,zero_cells,raw);
  return b58_normalize_commit_serial_b4(raw,leading,output);
}

int braid58_decode_64_avx2_b5(const char *input,size_t input_len,
                              uint8_t output[64]) {
  if (!input || !output || input_len<64 || input_len>88) return 0;
  _Alignas(32) uint8_t digit[160];
  const __m256i z=_mm256_setzero_si256();
  for (unsigned i=0;i<5;++i)
    _mm256_store_si256((__m256i *)(void *)(digit+32U*i),z);
  int valid;
  const size_t leading=b58_map_input(input,input_len,digit,90,&valid);
  if (!valid) return 0;
  _Alignas(32) uint32_t cell[24];
  b58_parse_b5(digit,cell);
  __m256i raw[4]; b58_matrix_b5(cell,raw);
  return b58_normalize_commit(raw,leading,output);
}
#endif

/* Final A/B selection: radix-58^4 with Boolean carry-lookahead. */
int braid58_decode_64_avx2(const char *input,size_t input_len,
                           uint8_t output[64]) {
  return b58_decode_64_b4_core(input,input_len,output);
}
