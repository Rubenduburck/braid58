/*
 * Fixed-32-byte Bitcoin Base58 decoder for AVX2-only CPUs.
 *
 * The decoder classifies and groups 48 padded digits with AVX2, then folds
 * eight radix-58^6 cells into four 64-bit limbs. The fixed input width lets
 * the Horner schedule skip limbs that are known to remain zero.
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

__extension__ typedef unsigned __int128 braid58_uint128;

static const uint8_t BRAID58_ASCII_ADJUST[32] __attribute__((aligned(32))) = {
    0, 0, 0, 0, 7, 9, 15, 15, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 7, 9, 15, 15, 0, 0, 0, 0, 0, 0, 0, 0,
};

static inline void braid58_store_be64(uint8_t *output, uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  value = __builtin_bswap64(value);
#else
  value = ((value & UINT64_C(0x00000000000000ff)) << 56) |
          ((value & UINT64_C(0x000000000000ff00)) << 40) |
          ((value & UINT64_C(0x0000000000ff0000)) << 24) |
          ((value & UINT64_C(0x00000000ff000000)) << 8) |
          ((value & UINT64_C(0x000000ff00000000)) >> 8) |
          ((value & UINT64_C(0x0000ff0000000000)) >> 24) |
          ((value & UINT64_C(0x00ff000000000000)) >> 40) |
          ((value & UINT64_C(0xff00000000000000)) >> 56);
#endif
  memcpy(output, &value, sizeof(value));
}

static inline uint64_t braid58_mul_add_b6(uint64_t *value, uint64_t carry) {
  const braid58_uint128 product = (braid58_uint128)*value * BRAID58_B6 + carry;
  *value = (uint64_t)product;
  return (uint64_t)(product >> 64);
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
