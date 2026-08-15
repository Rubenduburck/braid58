/*
 * Dedicated fixed-32-byte Bitcoin Base58 kernels for AVX2-only CPUs.
 *
 * The encoder retains the 2^26 -> 58^6 matrix used by the AVX-512 kernel,
 * split across two YMM registers. AVX2 lacks packed 64-bit multiplication,
 * so each 26x36-bit product is assembled from two 32-bit products.
 *
 * The decoder classifies and groups 48 padded digits with AVX2, then folds
 * the eight 58^6 cells into four 64-bit limbs. The fixed input width lets the
 * Horner schedule skip limbs that are known to remain zero.
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

#define BRAID58_B2 UINT64_C(3364)
#define BRAID58_B6 UINT64_C(38068692544)
#define BRAID58_M3 UINT64_C(195112)
#define BRAID58_CHUNK_MASK ((UINT32_C(1) << 26) - 1U)

__extension__ typedef unsigned __int128 braid58_uint128;

static const uint64_t BRAID58_W6[8][8] __attribute__((aligned(32))) = {
    {UINT64_C(35230722752), UINT64_C(118301), 0, 0, 0, 0, 0, 0},
    {UINT64_C(19472320256), UINT64_C(20819776900), UINT64_C(208), 0, 0, 0, 0,
     0},
    {UINT64_C(12663192832), UINT64_C(18874684076), UINT64_C(13995345565), 0, 0,
     0, 0, 0},
    {UINT64_C(36841335040), UINT64_C(27559759484), UINT64_C(32157257677),
     UINT64_C(24671499), 0, 0, 0, 0},
    {UINT64_C(35502351808), UINT64_C(32134872081), UINT64_C(17027758953),
     UINT64_C(30820324005), UINT64_C(43491), 0, 0, 0},
    {UINT64_C(249488768), UINT64_C(11869408826), UINT64_C(19667255935),
     UINT64_C(21282160635), UINT64_C(25465302058), UINT64_C(76), 0, 0},
    {UINT64_C(30339540544), UINT64_C(34170182847), UINT64_C(37217747693),
     UINT64_C(8289492547), UINT64_C(29132788384), UINT64_C(5145164816), 0, 0},
    {UINT64_C(5170572032), UINT64_C(19969761524), UINT64_C(27888303054),
     UINT64_C(14786692691), UINT64_C(34727737816), UINT64_C(2939016745),
     UINT64_C(9070082), 0},
};

static const uint8_t BRAID58_ASCII_ADJUST[32] __attribute__((aligned(32))) = {
    0, 0, 0, 0, 7, 9, 15, 15, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 7, 9, 15, 15, 0, 0, 0, 0, 0, 0, 0, 0,
};

static inline uint64_t braid58_load_be64(const uint8_t *input) {
  uint64_t value;
  memcpy(&value, input, sizeof(value));
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

static inline void braid58_store_be64(uint8_t *output, uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  value = __builtin_bswap64(value);
#else
  value = braid58_load_be64((const uint8_t *)&value);
#endif
  memcpy(output, &value, sizeof(value));
}

static inline uint64_t braid58_mul_add_b6(uint64_t *value, uint64_t carry) {
  const braid58_uint128 product = (braid58_uint128)*value * BRAID58_B6 + carry;
  *value = (uint64_t)product;
  return (uint64_t)(product >> 64);
}

BRAID58_TARGET_AVX2 static inline __m256i braid58_mul_u26_u36(__m256i weights,
                                                              uint32_t scalar) {
  const __m256i value = _mm256_set1_epi64x((long long)scalar);
  const __m256i low = _mm256_mul_epu32(value, weights);
  const __m256i high = _mm256_mul_epu32(value, _mm256_srli_epi64(weights, 32));
  return _mm256_add_epi64(low, _mm256_slli_epi64(high, 32));
}

BRAID58_TARGET_AVX2 static inline __m256i braid58_ascii(__m256i digits) {
  __m256i ascii = _mm256_add_epi8(digits, _mm256_set1_epi8('1'));
  const int threshold[5] = {8, 16, 21, 32, 43};
  const int adjustment[5] = {7, 1, 1, 6, 1};
  for (unsigned i = 0; i < 5; ++i) {
    const __m256i mask =
        _mm256_cmpgt_epi8(digits, _mm256_set1_epi8((char)threshold[i]));
    ascii = _mm256_add_epi8(
        ascii, _mm256_and_si256(mask, _mm256_set1_epi8((char)adjustment[i])));
  }
  return ascii;
}

BRAID58_TARGET_AVX2 size_t
braid58_encode_32_avx2(const uint8_t input[static 32],
                       char output[static BRAID58_ENCODED_32_CAPACITY]) {
  uint64_t words[4];
  uint32_t chunks[10];
  uint64_t columns[8];
  uint64_t cells[8];
  uint32_t high3[8];
  uint32_t low3[8];
  uint16_t lead[16];
  uint16_t middle[16];
  uint16_t low[16];
  uint8_t digits[64] = {0};
  uint8_t ascii[64];

  words[0] = braid58_load_be64(input + 24);
  words[1] = braid58_load_be64(input + 16);
  words[2] = braid58_load_be64(input + 8);
  words[3] = braid58_load_be64(input);
  for (unsigned i = 0; i < 10; ++i) {
    const unsigned bit = i * 26U;
    const unsigned word = bit >> 6;
    const unsigned shift = bit & 63U;
    uint64_t value = words[word] >> shift;
    if (shift > 38U && word < 3U)
      value |= words[word + 1U] << (64U - shift);
    chunks[i] = (uint32_t)value & BRAID58_CHUNK_MASK;
  }

  __m256i columns0 = _mm256_set_epi64x(
      0, 0, 0, (long long)((uint64_t)chunks[0] + ((uint64_t)chunks[1] << 26)));
  __m256i columns1 = _mm256_setzero_si256();
  for (unsigned i = 0; i < 8; ++i) {
    columns0 = _mm256_add_epi64(
        columns0, braid58_mul_u26_u36(
                      _mm256_load_si256((const __m256i *)&BRAID58_W6[i][0]),
                      chunks[i + 2U]));
    columns1 = _mm256_add_epi64(
        columns1, braid58_mul_u26_u36(
                      _mm256_load_si256((const __m256i *)&BRAID58_W6[i][4]),
                      chunks[i + 2U]));
  }
  _mm256_storeu_si256((__m256i *)&columns[0], columns0);
  _mm256_storeu_si256((__m256i *)&columns[4], columns1);

  uint64_t previous_quotient = 0;
  uint64_t carry = 0;
  for (unsigned i = 0; i < 8; ++i) {
    const uint64_t quotient = columns[i] / BRAID58_B6;
    uint64_t value = columns[i] - quotient * BRAID58_B6;
    value += previous_quotient + carry;
    carry = value >= BRAID58_B6;
    cells[i] = value - carry * BRAID58_B6;
    previous_quotient = quotient;

    high3[i] = (uint32_t)(cells[i] / BRAID58_M3);
    low3[i] = (uint32_t)(cells[i] - (uint64_t)high3[i] * BRAID58_M3);
  }

  const __m256i high_values = _mm256_loadu_si256((const __m256i *)high3);
  const __m256i low_values = _mm256_loadu_si256((const __m256i *)low3);
  const __m256i magic = _mm256_set1_epi64x(INT64_C(40855813));
  const __m256i high_even =
      _mm256_srli_epi64(_mm256_mul_epu32(high_values, magic), 37);
  const __m256i high_odd = _mm256_slli_epi64(
      _mm256_srli_epi64(
          _mm256_mul_epu32(_mm256_srli_epi64(high_values, 32), magic), 37),
      32);
  const __m256i low_even =
      _mm256_srli_epi64(_mm256_mul_epu32(low_values, magic), 37);
  const __m256i low_odd = _mm256_slli_epi64(
      _mm256_srli_epi64(
          _mm256_mul_epu32(_mm256_srli_epi64(low_values, 32), magic), 37),
      32);
  const __m256i high_lead = _mm256_blend_epi32(high_even, high_odd, 0xaa);
  const __m256i low_lead = _mm256_blend_epi32(low_even, low_odd, 0xaa);
  const __m256i high_pair = _mm256_sub_epi32(
      high_values, _mm256_mullo_epi32(high_lead, _mm256_set1_epi32(3364)));
  const __m256i low_pair = _mm256_sub_epi32(
      low_values, _mm256_mullo_epi32(low_lead, _mm256_set1_epi32(3364)));
  __m256i lead16 = _mm256_packus_epi32(high_lead, low_lead);
  __m256i pair16 = _mm256_packus_epi32(high_pair, low_pair);
  lead16 = _mm256_permute4x64_epi64(lead16, 0xd8);
  pair16 = _mm256_permute4x64_epi64(pair16, 0xd8);
  const __m256i middle16 = _mm256_mulhi_epu16(pair16, _mm256_set1_epi16(1130));
  const __m256i low16 = _mm256_sub_epi16(
      pair16, _mm256_mullo_epi16(middle16, _mm256_set1_epi16(58)));
  _mm256_storeu_si256((__m256i *)lead, lead16);
  _mm256_storeu_si256((__m256i *)middle, middle16);
  _mm256_storeu_si256((__m256i *)low, low16);

  for (unsigned i = 0; i < 8; ++i) {
    const unsigned cell = 7U - i;
    digits[6U * i] = (uint8_t)lead[cell];
    digits[6U * i + 1U] = (uint8_t)middle[cell];
    digits[6U * i + 2U] = (uint8_t)low[cell];
    digits[6U * i + 3U] = (uint8_t)lead[cell + 8U];
    digits[6U * i + 4U] = (uint8_t)middle[cell + 8U];
    digits[6U * i + 5U] = (uint8_t)low[cell + 8U];
  }

  const __m256i input0 =
      _mm256_loadu_si256((const __m256i *)(const void *)input);
  const uint32_t zero_mask = (uint32_t)_mm256_movemask_epi8(
      _mm256_cmpeq_epi8(input0, _mm256_setzero_si256()));
  const uint32_t nonzero_bytes = ~zero_mask;
  const unsigned leading_zero_bytes =
      nonzero_bytes ? (unsigned)__builtin_ctz(nonzero_bytes) : 32U;
  unsigned first_digit = 0;
  while (first_digit < 48U && digits[first_digit] == 0)
    ++first_digit;
  const unsigned skip = first_digit - leading_zero_bytes;
  const size_t length = 48U - skip;

  _mm256_storeu_si256(
      (__m256i *)(void *)ascii,
      braid58_ascii(_mm256_loadu_si256((const __m256i *)(const void *)digits)));
  _mm256_storeu_si256((__m256i *)(void *)(ascii + 16),
                      braid58_ascii(_mm256_loadu_si256(
                          (const __m256i *)(const void *)(digits + 16))));
  memcpy(output, ascii + skip, length);
  output[length] = '\0';
  return length;
}

BRAID58_TARGET_AVX2 static inline __m256i braid58_base58_digits(__m256i ascii,
                                                                int *valid) {
  const __m256i nibble =
      _mm256_and_si256(_mm256_srli_epi16(ascii, 4), _mm256_set1_epi8(15));
  __m256i adjustment = _mm256_shuffle_epi8(
      _mm256_load_si256((const __m256i *)BRAID58_ASCII_ADJUST), nibble);
  const __m256i upper_gap =
      _mm256_and_si256(_mm256_cmpgt_epi8(ascii, _mm256_set1_epi8('I')),
                       _mm256_cmpgt_epi8(_mm256_set1_epi8('P'), ascii));
  const __m256i lower_gap = _mm256_cmpgt_epi8(ascii, _mm256_set1_epi8('l'));
  adjustment =
      _mm256_sub_epi8(adjustment, _mm256_or_si256(upper_gap, lower_gap));
  const __m256i value = _mm256_sub_epi8(
      _mm256_sub_epi8(ascii, _mm256_set1_epi8('1')), adjustment);
  const __m256i in_range =
      _mm256_cmpeq_epi8(_mm256_min_epu8(value, _mm256_set1_epi8(57)), value);
  const __m256i round_trip = _mm256_cmpeq_epi8(braid58_ascii(value), ascii);
  *valid &= _mm256_movemask_epi8(_mm256_and_si256(in_range, round_trip)) == -1;
  return value;
}

BRAID58_TARGET_AVX2 int braid58_decode_32_avx2(const char *input, size_t length,
                                               uint8_t output[32]) {
  uint8_t digit[64] __attribute__((aligned(32)));
  uint16_t pair[32] __attribute__((aligned(32)));
  uint64_t cell[8];
  uint64_t limb[4] = {0};
  uint8_t decoded[32];

  if (input == NULL || output == NULL || length < 32U || length > 44U)
    return 0;

  size_t leading_ones = 0;
  while (leading_ones < length && input[leading_ones] == '1')
    ++leading_ones;

  int valid = 1;
  const __m256i digit0 = braid58_base58_digits(
      _mm256_loadu_si256((const __m256i *)(const void *)input), &valid);
  const __m256i digit1 = braid58_base58_digits(
      _mm256_loadu_si256((const __m256i *)(const void *)(input + length - 32U)),
      &valid);
  if (!valid)
    return 0;
  _mm256_store_si256((__m256i *)(void *)digit, _mm256_setzero_si256());
  _mm256_store_si256((__m256i *)(void *)(digit + 32), _mm256_setzero_si256());
  _mm256_storeu_si256((__m256i *)(void *)(digit + 48U - length), digit0);
  _mm256_storeu_si256((__m256i *)(void *)(digit + 16), digit1);
  const __m256i pair_weights = _mm256_set1_epi16((short)0x013a);
  _mm256_store_si256(
      (__m256i *)(void *)pair,
      _mm256_maddubs_epi16(
          _mm256_load_si256((const __m256i *)(const void *)digit),
          pair_weights));
  _mm256_store_si256(
      (__m256i *)(void *)(pair + 16),
      _mm256_maddubs_epi16(
          _mm256_load_si256((const __m256i *)(const void *)(digit + 32)),
          pair_weights));
  for (unsigned i = 0; i < 8; ++i) {
    cell[i] = (uint64_t)pair[3U * i] * UINT64_C(11316496) +
              (uint64_t)pair[3U * i + 1U] * BRAID58_B2 + pair[3U * i + 2U];
  }

  limb[0] = cell[0];
  uint64_t carry = braid58_mul_add_b6(&limb[0], cell[1]);
  limb[1] = carry;
  carry = braid58_mul_add_b6(&limb[0], cell[2]);
  limb[1] = carry;
  carry = braid58_mul_add_b6(&limb[0], cell[3]);
  carry = braid58_mul_add_b6(&limb[1], carry);
  limb[2] = carry;
  carry = braid58_mul_add_b6(&limb[0], cell[4]);
  carry = braid58_mul_add_b6(&limb[1], carry);
  limb[2] = carry;
  carry = braid58_mul_add_b6(&limb[0], cell[5]);
  carry = braid58_mul_add_b6(&limb[1], carry);
  carry = braid58_mul_add_b6(&limb[2], carry);
  limb[3] = carry;
  carry = braid58_mul_add_b6(&limb[0], cell[6]);
  carry = braid58_mul_add_b6(&limb[1], carry);
  carry = braid58_mul_add_b6(&limb[2], carry);
  limb[3] = carry;
  carry = braid58_mul_add_b6(&limb[0], cell[7]);
  carry = braid58_mul_add_b6(&limb[1], carry);
  carry = braid58_mul_add_b6(&limb[2], carry);
  carry = braid58_mul_add_b6(&limb[3], carry);
  if (carry != 0)
    return 0;

  braid58_store_be64(decoded, limb[3]);
  braid58_store_be64(decoded + 8, limb[2]);
  braid58_store_be64(decoded + 16, limb[1]);
  braid58_store_be64(decoded + 24, limb[0]);
  size_t leading_zero_bytes = 0;
  while (leading_zero_bytes < 32U && decoded[leading_zero_bytes] == 0)
    ++leading_zero_bytes;
  if (leading_ones != leading_zero_bytes)
    return 0;

  memcpy(output, decoded, sizeof(decoded));
  return 1;
}
