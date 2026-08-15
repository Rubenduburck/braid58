/*
 * Braid58: fixed-32-byte Bitcoin Base58 AVX-512 decoder.
 *
 * This research kernel is deliberately narrow:
 *   - input is an explicit-length, canonical Bitcoin Base58 string;
 *   - the accepted encoded length is 32..44 bytes;
 *   - successful inputs decode to exactly one 32-byte big-endian value;
 *   - AVX2 and AVX-512 F/DQ/BW/VL/IFMA/VBMI/VBMI2 are required.
 *
 * The 44 input digits are right-aligned in a 48-digit field and folded into
 * eight radix-(58^6) cells.  An IFMA matrix then maps those cells into five
 * radix-(2^52) limbs.  Overflow and Bitcoin's leading-'1' canonicality rule
 * are checked before the caller's output is touched.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define BRAID58_TARGET                                                        \
  __attribute__((target("avx2,avx512f,avx512dq,avx512bw,avx512vl,avx512ifma," \
                        "avx512vbmi,avx512vbmi2")))
#else
#define BRAID58_TARGET
#endif

#define BRAID58_B2 UINT64_C(3364)          /* 58^2 */
#define BRAID58_B4 UINT64_C(11316496)      /* 58^4 */
#define BRAID58_B6 UINT64_C(38068692544)   /* 58^6 */
#define BRAID58_Q_MASK UINT64_C(0x000fffffffffffff) /* 2^52 - 1 */

/* ASCII 0..127 -> Bitcoin Base58 digit.  0xff denotes an invalid byte. */
_Alignas(64) static const uint8_t BRAID58_INV_LO[64] = {
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255,   0,   1,   2,   3,   4,   5,   6,
      7,   8, 255, 255, 255, 255, 255, 255,
};

_Alignas(64) static const uint8_t BRAID58_INV_HI[64] = {
    255,   9,  10,  11,  12,  13,  14,  15,
     16, 255,  17,  18,  19,  20,  21, 255,
     22,  23,  24,  25,  26,  27,  28,  29,
     30,  31,  32, 255, 255, 255, 255, 255,
    255,  33,  34,  35,  36,  37,  38,  39,
     40,  41,  42,  43, 255,  44,  45,  46,
     47,  48,  49,  50,  51,  52,  53,  54,
     55,  56,  57, 255, 255, 255, 255, 255,
};

/* Transpose eight triples of radix-58^2 pair values into three blocks. */
_Alignas(64) static const uint16_t BRAID58_PAIR_TRANSPOSE[32] = {
     0, 3, 6, 9,12,15,18,21,
     1, 4, 7,10,13,16,19,22,
     2, 5, 8,11,14,17,20,23,
    31,31,31,31,31,31,31,31,
};

/*
 * W52[i][j] is digit j, in radix 2^52, of (58^6)^(7-i).  Lanes 5..7
 * are zero.  vpmadd52luq contributes a product's low digit to lane j;
 * vpmadd52huq is shifted one lane and contributes the high digit to j+1.
 */
_Alignas(64) static const uint64_t BRAID58_W52[8][8] = {
    {UINT64_C(1587694790508544), UINT64_C(2010566948527045),
     UINT64_C(4305213882740208), UINT64_C(2346159416487115),
     UINT64_C(281667430222), 0, 0, 0},
    {UINT64_C(656477161259008), UINT64_C(1474050348024598),
     UINT64_C(2194511462244540), UINT64_C(1796601940663378),
     UINT64_C(7), 0, 0, 0},
    {UINT64_C(2998773009612800), UINT64_C(1265285701333454),
     UINT64_C(741474571213368), UINT64_C(875307), 0, 0, 0, 0},
    {UINT64_C(514192964583424), UINT64_C(1445221437915584),
     UINT64_C(103550522938), 0, 0, 0, 0, 0},
    {UINT64_C(2263791264071680), UINT64_C(3243026990356132),
     UINT64_C(2), 0, 0, 0, 0, 0},
    {UINT64_C(3020718794543104), UINT64_C(321792), 0, 0, 0, 0, 0, 0},
    {UINT64_C(38068692544), 0, 0, 0, 0, 0, 0, 0},
    {UINT64_C(1), 0, 0, 0, 0, 0, 0, 0},
};

static inline void
braid58_store_be64(uint8_t *dst, uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  value = __builtin_bswap64(value);
#else
  value = ((value & UINT64_C(0x00000000000000ff)) << 56) |
          ((value & UINT64_C(0x000000000000ff00)) << 40) |
          ((value & UINT64_C(0x0000000000ff0000)) << 24) |
          ((value & UINT64_C(0x00000000ff000000)) << 8)  |
          ((value & UINT64_C(0x000000ff00000000)) >> 8)  |
          ((value & UINT64_C(0x0000ff0000000000)) >> 24) |
          ((value & UINT64_C(0x00ff000000000000)) >> 40) |
          ((value & UINT64_C(0xff00000000000000)) >> 56);
#endif
  memcpy(dst, &value, sizeof(value));
}

/*
 * Returns 1 on success and 0 on malformed, noncanonical, or overflowing
 * input.  On failure, out[0..31] is left unchanged.
 */
BRAID58_TARGET int
braid58_decode_32_avx512(const char *input, size_t input_len, uint8_t out[32]) {
  uint64_t low_scalar[8];
  uint64_t high_scalar[8];
  uint64_t limb[8];
  uint8_t decoded[32];
  size_t leading_ones = 0;

  if (input == NULL || out == NULL || input_len < 32 || input_len > 44)
    return 0;

  while (leading_ones < input_len &&
         (uint8_t)input[leading_ones] == (uint8_t)'1')
    ++leading_ones;

  /*
   * vpexpandb loads exactly input_len bytes, right-aligned in a 48-digit
   * field.  That field is eight consecutive six-digit records in lanes
   * 0..47; lanes 48..63 remain zero.
   */
  const __mmask64 active =
      ((UINT64_C(1) << (unsigned)input_len) - UINT64_C(1))
      << (48U - (unsigned)input_len);
  const __m512i ascii =
      _mm512_maskz_expandloadu_epi8(active, (const void *)input);
  const __m512i inv_lo =
      _mm512_load_si512((const void *)BRAID58_INV_LO);
  const __m512i inv_hi =
      _mm512_load_si512((const void *)BRAID58_INV_HI);
  const __m512i mapped = _mm512_permutex2var_epi8(inv_lo, ascii, inv_hi);

  /* vpermi2b indexes modulo 128, so reject high-ASCII separately. */
  const __mmask64 high_ascii = _mm512_movepi8_mask(ascii);
  const __mmask64 bad_table = _mm512_movepi8_mask(mapped);
  if (((high_ascii | bad_table) & active) != 0)
    return 0;

  /* Inactive/padding lanes must be numeric zero, not table sentinel 0xff. */
  const __m512i digits = _mm512_maskz_mov_epi8(active, mapped);

  /* 48 right-aligned digits -> 24 base-58^2 pairs -> eight base-58^6 cells. */
  const __m512i pair_weights = _mm512_set1_epi16((short)0x013a);
  __m512i pairs = _mm512_maddubs_epi16(digits, pair_weights);
  pairs = _mm512_permutexvar_epi16(
      _mm512_load_si512((const void *)BRAID58_PAIR_TRANSPOSE), pairs);
  const __m512i pair0 =
      _mm512_cvtepu16_epi64(_mm512_castsi512_si128(pairs));
  const __m512i pair1 =
      _mm512_cvtepu16_epi64(_mm512_extracti32x4_epi32(pairs, 1));
  const __m512i pair2 =
      _mm512_cvtepu16_epi64(_mm512_extracti32x4_epi32(pairs, 2));
  const __m512i cells = _mm512_add_epi64(
      _mm512_add_epi64(
          _mm512_mullo_epi64(pair0,
                             _mm512_set1_epi64((long long)BRAID58_B4)),
          _mm512_mullo_epi64(pair1,
                             _mm512_set1_epi64((long long)BRAID58_B2))),
      pair2);

  /*
   * Matrix conversion: radix 58^6 -> radix 2^52.  Keep the IFMA low and
   * high halves in separate vectors; scalar normalization folds high[j-1]
   * into low[j], avoiding eight vector shift/add operations in the hot path.
   */
  __m512i low = _mm512_setzero_si512();
  __m512i high = _mm512_setzero_si512();
  for (unsigned i = 0; i < 8; ++i) {
    const __m512i a = _mm512_permutexvar_epi64(
        _mm512_set1_epi64((long long)i), cells);
    const __m512i weight =
        _mm512_load_si512((const void *)BRAID58_W52[i]);
    low = _mm512_madd52lo_epu64(low, a, weight);
    high = _mm512_madd52hi_epu64(high, a, weight);
  }
  _mm512_storeu_si512((void *)low_scalar, low);
  _mm512_storeu_si512((void *)high_scalar, high);

  /* Exact radix-2^52 normalization. */
  uint64_t carry = 0;
  for (unsigned i = 0; i < 5; ++i) {
    const uint64_t sum =
        low_scalar[i] + (i != 0 ? high_scalar[i - 1] : 0) + carry;
    limb[i] = sum & BRAID58_Q_MASK;
    carry = sum >> 52;
  }

  /* Lane five records any >260-bit carry; only 48 bits of limb four fit. */
  if (high_scalar[4] + carry != 0 || (limb[4] >> 48) != 0)
    return 0;

  /* Five 52-bit limbs -> four little-endian 64-bit words -> big-endian bytes. */
  const uint64_t word0 = limb[0] | (limb[1] << 52);
  const uint64_t word1 = (limb[1] >> 12) | (limb[2] << 40);
  const uint64_t word2 = (limb[2] >> 24) | (limb[3] << 28);
  const uint64_t word3 = (limb[3] >> 36) | (limb[4] << 16);
  braid58_store_be64(decoded + 0, word3);
  braid58_store_be64(decoded + 8, word2);
  braid58_store_be64(decoded + 16, word1);
  braid58_store_be64(decoded + 24, word0);

  /* In Bitcoin Base58, each leading '1' represents exactly one zero byte. */
  size_t leading_zero_bytes = 0;
  while (leading_zero_bytes < 32 && decoded[leading_zero_bytes] == 0)
    ++leading_zero_bytes;
  if (leading_ones != leading_zero_bytes)
    return 0;

  memcpy(out, decoded, sizeof(decoded));
  return 1;
}
