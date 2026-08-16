/*
 * AVX-512 Bitcoin Base58 encoder for 64-byte inputs.
 *
 * The selected path splits the input into twenty little-endian radix-2^26
 * chunks and maps them into seventeen raw radix-58^5 columns.  Every complete
 * raw column is below 2^59, so ZMM VPMULUDQ can accumulate the basis transform
 * exactly without split products or bank normalization.  Two accumulator
 * banks shorten the longest dependency chain.  An eighteenth normalized
 * cell is the bounded carry above raw column sixteen.
 *
 * File-local B6/VPMULLQ and B6/IFMA bodies are retained for comparison.
 * Dead-code elimination removes them from the production object. The object
 * audit rejects IFMA, VBMI, and VPMULLQ in the selected B5 object.
 */

#include "braid58_internal.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { BRAID58_CHUNK_MASK = (1U << 26) - 1U };

static const uint64_t BRAID58_B6 = UINT64_C(38068692544); /* 58^6 */
static const uint64_t BRAID58_B3 = UINT64_C(195112);      /* 58^3 */
static const uint64_t BRAID58_B5 = UINT64_C(656356768);   /* 58^5 */

/* Little-endian radix-58^6 expansion of 2^(26*i), padded to two ZMMs. */
_Alignas(64) static const uint64_t BRAID58_W6[20][16] = {
  {1,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
  {67108864,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
  {35230722752,118301,0,0,0,0,0,0, 0,0,0,0,0,0,0,0},
  {19472320256,20819776900,208,0,0,0,0,0, 0,0,0,0,0,0,0,0},
  {12663192832,18874684076,13995345565,0,0,0,0,0, 0,0,0,0,0,0,0,0},
  {36841335040,27559759484,32157257677,24671499,0,0,0,0, 0,0,0,0,0,0,0,0},
  {35502351808,32134872081,17027758953,30820324005,43491,0,0,0, 0,0,0,0,0,0,0,0},
  {249488768,11869408826,19667255935,21282160635,25465302058,76,0,0, 0,0,0,0,0,0,0,0},
  {30339540544,34170182847,37217747693,8289492547,29132788384,5145164816,0,0, 0,0,0,0,0,0,0,0},
  {5170572032,19969761524,27888303054,14786692691,34727737816,2939016745,9070082,0, 0,0,0,0,0,0,0,0},
  {31689162368,28101825846,11270318807,3630711370,20139227987,26749912812,2579501836,15989, 0,0,0,0,0,0,0,0},
  {5918451840,37083182158,15600094303,35385846092,12569159708,35263841485,32600636542,7084782502, 28,0,0,0,0,0,0,0},
  {12449496320,28690307692,30046015410,20170101816,37593259509,15566430583,26625938771,2977062665, 1891537502,0,0,0,0,0,0,0},
  {1917195456,37779880757,16986850292,31615538615,19125378726,6461339105,610809449,16066432013, 19750674121,3334470,0,0,0,0,0,0},
  {10858472640,6365541365,9672546616,20920358408,11194806867,21910276763,35145338953,27379688148, 21479874229,4753785648,5878,0,0,0,0,0},
  {5095148352,32912596747,37068024157,7792411497,9083592954,37234983441,33220387937,22820357198, 33169269933,29164203032,13787357296,10,0,0,0,0},
  {9497620544,15109328791,26043741304,37626610065,8387651559,20158292885,30732107971,22387019057, 5550834820,29987442481,23770045052,695393490,0,0,0,0},
  {37525923648,24595511835,12439021701,2490614526,24153667145,23605582534,5836871317,18521382580, 30024142340,37068347259,22207475424,27472040036,1225864,0,0,0},
  {15894008128,7449224371,586455455,3818089940,16799652305,4785988363,9148883492,4042378983, 30804835761,9892075480,18885820683,31874629354,38012992158,2160,0,0},
  {25772006272,13176653090,31384176625,23953907680,33901540778,33238980536,27980436336,16850869549, 12311603555,14186984387,28027435099,1801832637,13416956146,30816079281,3,0}
};

/* B5 basis split into the two active eight-column ZMM blocks. */
_Alignas(64) static const uint64_t BRAID58_W5_LO[18][8] = {
  {443814048,6861511,0,0,0,0,0,0},
  {437973984,506963877,701551,0,0,0,0,0},
  {192414240,584944171,527870455,71729,0,0,0,0},
  {85356032,237320048,230466711,641497958,7333,0,0,0},
  {59086336,425716400,469620603,369434287,563670112,749,0,0},
  {249488768,563819044,543108756,589143927,28959436,439056932,76,0},
  {147129216,329522580,449746271,218521078,590921969,502289454,550667440,7},
  {576074656,432829647,553198108,378465102,185459504,598754100,313589673,526064760},
  {184037504,172044172,216479247,655243859,487688611,347228068,495642092,618120192},
  {11240928,599793205,368209896,143682386,101050947,216709650,476932781,529427649},
  {635074496,173439274,448067151,363842761,469972204,648159646,470224919,553330405},
  {604481920,314192324,31450010,106032878,54664182,329747909,554128193,640039347},
  {356764352,328895570,216399954,459733446,334397277,193013911,250503419,441894667},
  {500650976,245129989,205412112,298516231,409710268,156613671,479004433,375386322},
  {308625792,104784612,644355351,184514320,45134568,144614682,467589845,453637228},
  {113587872,276429623,155974033,608422969,645780644,416442536,633095654,514800901},
  {141445696,172260198,484063438,78047805,442478329,289649177,191490987,298973999},
  {174092320,246601307,71321328,139080596,505046486,584509323,421142136,351377042}
};

_Alignas(64) static const uint64_t BRAID58_W5_HI[10][8] = {
  {53787223,0,0,0,0,0,0,0},
  {237736760,5499447,0,0,0,0,0,0},
  {147241268,130740298,562288,0,0,0,0,0},
  {435587593,205535280,571695987,57490,0,0,0,0},
  {38393073,171253552,345880098,81961821,5878,0,0,0},
  {194031727,230544232,507785892,502831086,3865168,601,0,0},
  {212906911,527697587,239296485,517024870,141201404,295059608,61,0},
  {608440467,323200398,337103515,639109435,547702080,400446185,185668315,6},
  {163380196,293664278,348652084,170553025,507831179,427843872,341939960,421636746},
  {72896988,529815445,60242290,244603179,460450843,145566876,267323783,495067909}
};

_Alignas(32) static const uint8_t BRAID58_MAP0[] =
    "123456789ABCDEFG123456789ABCDEFG";
_Alignas(32) static const uint8_t BRAID58_MAP1[] =
    "HJKLMNPQRSTUVWXYHJKLMNPQRSTUVWXY";
_Alignas(32) static const uint8_t BRAID58_MAP2[] =
    "ZabcdefghijkmnopZabcdefghijkmnop";
_Alignas(32) static const uint8_t BRAID58_MAP3[] =
    "qrstuvwxyz\0\0\0\0\0\0qrstuvwxyz\0\0\0\0\0\0";

_Alignas(32) static const uint8_t BRAID58_FIELDS_A[32] = {
   4,5,6,7,12,13,14,15, 0x80,0x80,0x80,0x80,0,1,2,3,
   4,5,6,7,12,13,14,15, 0x80,0x80,0x80,0x80,0,1,2,3
};
_Alignas(32) static const uint8_t BRAID58_FIELDS_B[32] = {
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,4,5,6,7,0x80,0x80,0x80,0x80,
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,4,5,6,7,0x80,0x80,0x80,0x80
};
_Alignas(32) static const uint8_t BRAID58_HEAD_A[32] = {
  0x80,15,11,7,3,0x80,14,10,6,2,0x80,13,9,5,1,0x80,
  0x80,15,11,7,3,0x80,14,10,6,2,0x80,13,9,5,1,0x80
};
_Alignas(32) static const uint8_t BRAID58_HEAD_B[32] = {
  3,0x80,0x80,0x80,0x80,2,0x80,0x80,0x80,0x80,1,0x80,0x80,0x80,0x80,0,
  3,0x80,0x80,0x80,0x80,2,0x80,0x80,0x80,0x80,1,0x80,0x80,0x80,0x80,0
};
_Alignas(32) static const uint8_t BRAID58_TAIL_A[32] = {
  12,8,4,0,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
  12,8,4,0,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80
};

_Alignas(64) static const uint64_t BRAID58_PREV_LO[8] = {
  0,0,1,2,3,4,5,6
};

_Alignas(64) static const uint64_t BRAID58_PREV_HI[8] = {
  7,8,9,10,11,12,13,14
};

/* Big-endian order for the six digits in cells 7..0. */
_Alignas(64) static const uint8_t BRAID58_ORDER6[64] = {
   7,23,39,15,31,47, 6,22,38,14,30,46,
   5,21,37,13,29,45, 4,20,36,12,28,44,
   3,19,35,11,27,43, 2,18,34,10,26,42,
   1,17,33, 9,25,41, 0,16,32, 8,24,40,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

_Alignas(64) static const uint8_t BRAID58_ALPHABET[64] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* Indices for the first 64 output bytes after skipping 8 or 9 leading
 * padding digits from [high48 || low48]. */
_Alignas(64) static const uint8_t BRAID58_JOIN_SKIP8[64] = {
   8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,
  24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,
  40,41,42,43,44,45,46,47,64,65,66,67,68,69,70,71,
  72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87
};

_Alignas(64) static const uint8_t BRAID58_JOIN_SKIP9[64] = {
   9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
  25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  41,42,43,44,45,46,47,64,65,66,67,68,69,70,71,72,
  73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88
};

_Alignas(64) static const uint8_t BRAID58_CHUNK_INDEX[3][64] = {
  {
    63,62,61,60,59,58,57,56, 60,59,58,57,56,55,54,53,
    57,56,55,54,53,52,51,50, 54,53,52,51,50,49,48,47,
    50,49,48,47,46,45,44,43, 47,46,45,44,43,42,41,40,
    44,43,42,41,40,39,38,37, 41,40,39,38,37,36,35,34
  },
  {
    37,36,35,34,33,32,31,30, 34,33,32,31,30,29,28,27,
    31,30,29,28,27,26,25,24, 28,27,26,25,24,23,22,21,
    24,23,22,21,20,19,18,17, 21,20,19,18,17,16,15,14,
    18,17,16,15,14,13,12,11, 15,14,13,12,11,10, 9, 8
  },
  {
    11,10, 9, 8, 7, 6, 5, 4,  8, 7, 6, 5, 4, 3, 2, 1,
     5, 4, 3, 2, 1, 0, 0, 0,  2, 1, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0
  }
};

_Alignas(64) static const uint64_t BRAID58_CHUNK_SHIFT[8] = {
  0,2,4,6,0,2,4,6
};

typedef struct {
  __m512i lo;
  __m512i hi;
} braid58_cells15;

static inline uint64_t
braid58_load_be64(const void *pointer) {
  uint64_t value;
  memcpy(&value, pointer, sizeof(value));
  return __builtin_bswap64(value);
}

static inline unsigned
braid58_prefix(unsigned generate, unsigned propagate) {
  generate |= propagate & (generate << 1);
  propagate &= propagate << 1;
  generate |= propagate & (generate << 2);
  propagate &= propagate << 2;
  generate |= propagate & (generate << 4);
  propagate &= propagate << 4;
  generate |= propagate & (generate << 8);
  return generate & 0x7fffU;
}

static inline unsigned
braid58_prefix17(unsigned generate, unsigned propagate) {
  generate |= propagate & (generate << 1);
  propagate &= propagate << 1;
  generate |= propagate & (generate << 2);
  propagate &= propagate << 2;
  generate |= propagate & (generate << 4);
  propagate &= propagate << 4;
  generate |= propagate & (generate << 8);
  propagate &= propagate << 8;
  generate |= propagate & (generate << 16);
  return generate & 0x1ffffU;
}

/* Exact floor(x/58^6) and remainder for eight unsigned 64-bit lanes. */
static inline void
braid58_div_b6(__m512i x, __m512i *quotient, __m512i *remainder) {
  enum {
    rn_sae = _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC,
    rz_sae = _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC
  };
  const __m512i divisor = _mm512_set1_epi64((long long)BRAID58_B6);
  const __m512i one = _mm512_set1_epi64(1);
  const __m512d as_double = _mm512_cvt_roundepu64_pd(x, rn_sae);
  __m512i q = _mm512_cvt_roundpd_epu64(
      _mm512_mul_round_pd(
          as_double, _mm512_set1_pd(1.0 / 38068692544.0), rn_sae),
      rz_sae);
  __m512i product = _mm512_mullo_epi64(q, divisor);
  const __mmask8 too_high =
      _mm512_cmp_epu64_mask(product, x, _MM_CMPINT_GT);
  if (__builtin_expect(too_high != 0, 0)) {
    q = _mm512_mask_sub_epi64(q, too_high, q, one);
    product = _mm512_mask_sub_epi64(product, too_high, product, divisor);
  }
  __m512i r = _mm512_sub_epi64(x, product);
  const __mmask8 too_low =
      _mm512_cmp_epu64_mask(r, divisor, _MM_CMPINT_NLT);
  if (__builtin_expect(too_low != 0, 0)) {
    q = _mm512_mask_add_epi64(q, too_low, q, one);
    r = _mm512_mask_sub_epi64(r, too_low, r, divisor);
  }
  *quotient = q;
  *remainder = r;
}

/* Finish a fifteen-cell carry after an exact per-column quotient/remainder. */
static inline braid58_cells15
braid58_finish_qr(__m512i qlo, __m512i rlo,
                  __m512i qhi, __m512i rhi) {
  const __m512i zero = _mm512_setzero_si512();
  const __m512i one = _mm512_set1_epi64(1);
  const __m512i divisor = _mm512_set1_epi64((long long)BRAID58_B6);
  const __m512i previous_lo = _mm512_maskz_permutexvar_epi64(
      (__mmask8)0xfe, _mm512_load_si512(BRAID58_PREV_LO), qlo);
  const __m512i previous_hi = _mm512_permutex2var_epi64(
      qlo, _mm512_load_si512(BRAID58_PREV_HI), qhi);
  __m512i slo = _mm512_add_epi64(rlo, previous_lo);
  __m512i shi = _mm512_mask_add_epi64(
      zero, (__mmask8)0x7f, rhi, previous_hi);
  unsigned generate =
      (unsigned)_mm512_cmp_epu64_mask(slo, divisor, _MM_CMPINT_NLT);
  generate |= ((unsigned)_mm512_cmp_epu64_mask(
      shi, divisor, _MM_CMPINT_NLT) & 0x7fU) << 8;
  unsigned propagate = (unsigned)_mm512_cmpeq_epi64_mask(
      slo, _mm512_set1_epi64((long long)(BRAID58_B6 - 1U)));
  propagate |= ((unsigned)_mm512_cmpeq_epi64_mask(
      shi, _mm512_set1_epi64((long long)(BRAID58_B6 - 1U))) & 0x7fU) << 8;
  if (__builtin_expect(propagate != 0U, 0))
    generate = braid58_prefix(generate, propagate);
  else
    generate &= 0x7fffU;
  const unsigned carry_in = (generate << 1) & 0x7fffU;
  slo = _mm512_mask_add_epi64(
      slo, (__mmask8)carry_in, slo, one);
  shi = _mm512_mask_add_epi64(
      shi, (__mmask8)(carry_in >> 8), shi, one);
  slo = _mm512_mask_sub_epi64(
      slo, (__mmask8)generate, slo, divisor);
  shi = _mm512_mask_sub_epi64(
      shi, (__mmask8)(generate >> 8), shi, divisor);
  braid58_cells15 result = {slo, _mm512_maskz_mov_epi64((__mmask8)0x7f, shi)};
  return result;
}

static inline braid58_cells15
braid58_normalize_bank(__m512i raw_lo, __m512i raw_hi) {
  __m512i qlo, rlo, qhi, rhi;
  braid58_div_b6(raw_lo, &qlo, &rlo);
  braid58_div_b6(raw_hi, &qhi, &rhi);
  return braid58_finish_qr(qlo, rlo, qhi, rhi);
}

/* Bank A has no raw columns above seven, so avoid a second vector division. */
static inline braid58_cells15
braid58_normalize_low_bank(__m512i raw_lo) {
  __m512i qlo, rlo;
  braid58_div_b6(raw_lo, &qlo, &rlo);
  return braid58_finish_qr(
      qlo, rlo, _mm512_setzero_si512(), _mm512_setzero_si512());
}

/* Add two already normalized fifteen-cell radix vectors. */
static inline braid58_cells15
braid58_add_cells(braid58_cells15 a, braid58_cells15 b) {
  const __m512i divisor = _mm512_set1_epi64((long long)BRAID58_B6);
  const __m512i zero = _mm512_setzero_si512();
  __m512i slo = _mm512_add_epi64(a.lo, b.lo);
  __m512i shi = _mm512_add_epi64(a.hi, b.hi);
  const __mmask8 qlo_mask =
      _mm512_cmp_epu64_mask(slo, divisor, _MM_CMPINT_NLT);
  const __mmask8 qhi_mask = (__mmask8)(
      _mm512_cmp_epu64_mask(shi, divisor, _MM_CMPINT_NLT) & 0x7fU);
  const __m512i rlo = _mm512_mask_sub_epi64(slo, qlo_mask, slo, divisor);
  const __m512i rhi = _mm512_mask_sub_epi64(shi, qhi_mask, shi, divisor);
  const __m512i qlo = _mm512_maskz_set1_epi64(qlo_mask, 1);
  const __m512i qhi = _mm512_maskz_set1_epi64(qhi_mask, 1);
  (void)zero;
  return braid58_finish_qr(qlo, rlo, qhi, rhi);
}

static inline __attribute__((unused)) braid58_cells15
braid58_make_cells_banked(const uint8_t input[64]) {
  uint64_t word[8];
  uint32_t chunk[20];
  for (unsigned i = 0; i < 8; ++i) {
    word[i] = braid58_load_be64(input + 56U - 8U * i);
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(word[i]));
#endif
  }
#pragma GCC unroll 20
  for (unsigned i = 0; i < 20; ++i) {
    const unsigned bit = 26U * i;
    const unsigned wi = bit >> 6;
    const unsigned offset = bit & 63U;
    uint64_t value = word[wi] >> offset;
    if (offset > 38U && wi < 7U)
      value |= word[wi + 1U] << (64U - offset);
    chunk[i] = (uint32_t)(value & BRAID58_CHUNK_MASK);
  }

  const uint64_t low52 =
      (uint64_t)chunk[0] | ((uint64_t)chunk[1] << 26);
  __m512i bank_a_lo = _mm512_mask_set1_epi64(
      _mm512_setzero_si512(), (__mmask8)1, (long long)low52);
  __m512i bank_b_lo = _mm512_setzero_si512();
  __m512i bank_b_hi = _mm512_setzero_si512();

#define BRAID58_ADD_A(ROW) do {                                             \
    bank_a_lo = _mm512_add_epi64(                                          \
        bank_a_lo, _mm512_mullo_epi64(                                     \
            _mm512_set1_epi64((long long)chunk[(ROW)]),                    \
            _mm512_load_si512((const void *)&BRAID58_W6[(ROW)][0])));      \
    __asm__("" : "+v"(bank_a_lo));                                      \
  } while (0)
#define BRAID58_ADD_B(ROW) do {                                             \
    const __m512i value = _mm512_set1_epi64((long long)chunk[(ROW)]);       \
    bank_b_lo = _mm512_add_epi64(                                          \
        bank_b_lo, _mm512_mullo_epi64(                                     \
            value, _mm512_load_si512((const void *)&BRAID58_W6[(ROW)][0]))); \
    bank_b_hi = _mm512_add_epi64(                                          \
        bank_b_hi, _mm512_mullo_epi64(                                     \
            value, _mm512_load_si512((const void *)&BRAID58_W6[(ROW)][8]))); \
    __asm__("" : "+v"(bank_b_lo), "+v"(bank_b_hi));                    \
  } while (0)

  /* Three interleaved accumulator chains. */
  BRAID58_ADD_A(2);  BRAID58_ADD_B(11);
  BRAID58_ADD_A(3);  BRAID58_ADD_B(12);
  BRAID58_ADD_A(4);  BRAID58_ADD_B(13);
  BRAID58_ADD_A(5);  BRAID58_ADD_B(14);
  BRAID58_ADD_A(6);  BRAID58_ADD_B(15);
  BRAID58_ADD_A(7);  BRAID58_ADD_B(16);
  BRAID58_ADD_A(8);  BRAID58_ADD_B(17);
  BRAID58_ADD_A(9);  BRAID58_ADD_B(18);
  BRAID58_ADD_A(10); BRAID58_ADD_B(19);

#undef BRAID58_ADD_B
#undef BRAID58_ADD_A

  return braid58_add_cells(
      braid58_normalize_low_bank(bank_a_lo),
      braid58_normalize_bank(bank_b_lo, bank_b_hi));
}

/*
 * IFMA variant of the same basis transform.  VPMADD52LO accumulates the low
 * 52 product bits and VPMADD52HI the remaining high product bits.  With only
 * eighteen matrix rows, the former is below 2^57 and the latter below 2^15.
 * Canonicalizing that redundant pair produces an exact 65-bit column without
 * the two-bank normalization overhead.
 */
static inline void
braid58_div_b6_wide(__m512i sum_low, __m512i sum_high,
                    __m512i *quotient, __m512i *remainder) {
  const __m512i mask52 =
      _mm512_set1_epi64((long long)((UINT64_C(1) << 52) - 1U));
  const __m512i low52 = _mm512_and_si512(sum_low, mask52);
  const __m512i high = _mm512_add_epi64(
      sum_high, _mm512_srli_epi64(sum_low, 52));
  enum {
    rn_sae = _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC,
    rz_sae = _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC
  };
  const __m512d value = _mm512_fmadd_round_pd(
      _mm512_cvt_roundepu64_pd(high, rn_sae),
      _mm512_set1_pd(4503599627370496.0),
      _mm512_cvt_roundepu64_pd(low52, rn_sae), rn_sae);
  __m512i q = _mm512_cvt_roundpd_epu64(
      _mm512_mul_round_pd(
          value, _mm512_set1_pd(1.0 / 38068692544.0), rn_sae),
      rz_sae);
  const __m512i divisor = _mm512_set1_epi64((long long)BRAID58_B6);
  __m512i product_low = _mm512_madd52lo_epu64(
      _mm512_setzero_si512(), q, divisor);
  __m512i product_high = _mm512_madd52hi_epu64(
      _mm512_setzero_si512(), q, divisor);
  const __mmask8 high_greater =
      _mm512_cmp_epu64_mask(product_high, high, _MM_CMPINT_GT);
  const __mmask8 high_equal = _mm512_cmpeq_epi64_mask(product_high, high);
  const __mmask8 low_greater =
      _mm512_cmp_epu64_mask(product_low, low52, _MM_CMPINT_GT);
  const __mmask8 too_high = high_greater | (high_equal & low_greater);
  q = _mm512_mask_sub_epi64(
      q, too_high, q, _mm512_set1_epi64(1));

  /* Recompute after the possible one-step correction.  This is cheaper than
   * a base-2^52 masked subtract with borrow on the target instruction mix. */
  product_low = _mm512_madd52lo_epu64(
      _mm512_setzero_si512(), q, divisor);
  product_high = _mm512_madd52hi_epu64(
      _mm512_setzero_si512(), q, divisor);
  const __mmask8 borrow =
      _mm512_cmp_epu64_mask(low52, product_low, _MM_CMPINT_LT);
  const __m512i rlow = _mm512_and_si512(
      _mm512_sub_epi64(low52, product_low), mask52);
  const __m512i rhigh = _mm512_sub_epi64(
      _mm512_sub_epi64(high, product_high),
      _mm512_maskz_set1_epi64(borrow, 1));
  const __mmask8 too_low =
      _mm512_cmpneq_epi64_mask(rhigh, _mm512_setzero_si512()) |
      _mm512_cmp_epu64_mask(rlow, divisor, _MM_CMPINT_NLT);
  q = _mm512_mask_add_epi64(
      q, too_low, q, _mm512_set1_epi64(1));
  *quotient = q;
  *remainder = _mm512_mask_sub_epi64(rlow, too_low, rlow, divisor);
}

static inline __attribute__((unused)) braid58_cells15
braid58_make_cells_ifma(const uint8_t input[64]) {
  uint64_t word[8];
  uint32_t chunk[20];
  for (unsigned i = 0; i < 8; ++i) {
    word[i] = braid58_load_be64(input + 56U - 8U * i);
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(word[i]));
#endif
  }
#pragma GCC unroll 20
  for (unsigned i = 0; i < 20; ++i) {
    const unsigned bit = 26U * i;
    const unsigned wi = bit >> 6;
    const unsigned offset = bit & 63U;
    uint64_t value = word[wi] >> offset;
    if (offset > 38U && wi < 7U)
      value |= word[wi + 1U] << (64U - offset);
    chunk[i] = (uint32_t)(value & BRAID58_CHUNK_MASK);
  }

  const uint64_t low52 =
      (uint64_t)chunk[0] | ((uint64_t)chunk[1] << 26);
  __m512i low0 = _mm512_mask_set1_epi64(
      _mm512_setzero_si512(), (__mmask8)1, (long long)low52);
  __m512i high0 = _mm512_setzero_si512();
  __m512i low1 = _mm512_setzero_si512();
  __m512i high1 = _mm512_setzero_si512();

#define BRAID58_IFMA0(ROW) do {                                             \
    const __m512i value = _mm512_set1_epi64((long long)chunk[(ROW)]);       \
    const __m512i weight =                                                   \
        _mm512_load_si512((const void *)&BRAID58_W6[(ROW)][0]);             \
    low0 = _mm512_madd52lo_epu64(low0, value, weight);                      \
    high0 = _mm512_madd52hi_epu64(high0, value, weight);                    \
  } while (0)
#define BRAID58_IFMA1(ROW) do {                                             \
    const __m512i value = _mm512_set1_epi64((long long)chunk[(ROW)]);       \
    const __m512i weight =                                                   \
        _mm512_load_si512((const void *)&BRAID58_W6[(ROW)][8]);             \
    low1 = _mm512_madd52lo_epu64(low1, value, weight);                      \
    high1 = _mm512_madd52hi_epu64(high1, value, weight);                    \
  } while (0)

  BRAID58_IFMA0(2);  BRAID58_IFMA0(3);  BRAID58_IFMA0(4);
  BRAID58_IFMA0(5);  BRAID58_IFMA0(6);  BRAID58_IFMA0(7);
  BRAID58_IFMA0(8);  BRAID58_IFMA0(9);  BRAID58_IFMA0(10);
  BRAID58_IFMA0(11); BRAID58_IFMA1(11);
  BRAID58_IFMA0(12); BRAID58_IFMA1(12);
  BRAID58_IFMA0(13); BRAID58_IFMA1(13);
  BRAID58_IFMA0(14); BRAID58_IFMA1(14);
  BRAID58_IFMA0(15); BRAID58_IFMA1(15);
  BRAID58_IFMA0(16); BRAID58_IFMA1(16);
  BRAID58_IFMA0(17); BRAID58_IFMA1(17);
  BRAID58_IFMA0(18); BRAID58_IFMA1(18);
  BRAID58_IFMA0(19); BRAID58_IFMA1(19);

#undef BRAID58_IFMA1
#undef BRAID58_IFMA0

  __m512i qlo, rlo, qhi, rhi;
  braid58_div_b6_wide(low0, high0, &qlo, &rlo);
  braid58_div_b6_wide(low1, high1, &qhi, &rhi);
  return braid58_finish_qr(qlo, rlo, qhi, rhi);
}

typedef struct {
  __m256i packed[2];
  uint32_t top;
  uint32_t cell16;
} braid58_b5_cells;

static inline void
braid58_div_b5(__m512i x, __m512i *quotient, __m512i *remainder) {
  const __m512i magic = _mm512_set1_epi64(INT64_C(3513093979));
  const __m512i divisor = _mm512_set1_epi64((long long)BRAID58_B5);
  const __m512i p0 = _mm512_mul_epu32(x, magic);
  const __m512i p1 = _mm512_mul_epu32(_mm512_srli_epi64(x, 32), magic);
  __m512i q = _mm512_srli_epi64(
      _mm512_add_epi64(p1, _mm512_srli_epi64(p0, 32)), 29);
  __m512i product = _mm512_mul_epu32(q, divisor);
  const __mmask8 too_high =
      _mm512_cmp_epu64_mask(product, x, _MM_CMPINT_GT);
  q = _mm512_mask_sub_epi64(
      q, too_high, q, _mm512_set1_epi64(1));
  product = _mm512_mask_sub_epi64(
      product, too_high, product, divisor);
  *quotient = q;
  *remainder = _mm512_sub_epi64(x, product);
}

static inline uint64_t
braid58_lane7(__m512i value) {
  return (uint64_t)_mm_extract_epi64(
      _mm512_extracti64x2_epi64(value, 3), 1);
}

static inline braid58_b5_cells
braid58_finish_b5_raw(__m512i raw_lo, __m512i raw_hi, uint64_t raw16) {
  __m512i qlo, rlo, qhi, rhi;
  braid58_div_b5(raw_lo, &qlo, &rlo);
  braid58_div_b5(raw_hi, &qhi, &rhi);
  const __m512i previous_lo = _mm512_maskz_permutexvar_epi64(
      (__mmask8)0xfe, _mm512_load_si512(BRAID58_PREV_LO), qlo);
  const __m512i previous_hi = _mm512_permutex2var_epi64(
      qlo, _mm512_load_si512(BRAID58_PREV_HI), qhi);
  __m512i slo = _mm512_add_epi64(rlo, previous_lo);
  __m512i shi = _mm512_add_epi64(rhi, previous_hi);
  const uint64_t q16 = raw16 / BRAID58_B5;
  const uint64_t r16 = raw16 - q16 * BRAID58_B5;
  uint64_t sum16 = r16 + braid58_lane7(qhi);
  const __m512i divisor = _mm512_set1_epi64((long long)BRAID58_B5);
  unsigned generate =
      (unsigned)_mm512_cmp_epu64_mask(slo, divisor, _MM_CMPINT_NLT);
  generate |= (unsigned)_mm512_cmp_epu64_mask(
      shi, divisor, _MM_CMPINT_NLT) << 8;
  generate |= (unsigned)(sum16 >= BRAID58_B5) << 16;
  unsigned propagate = (unsigned)_mm512_cmpeq_epi64_mask(
      slo, _mm512_set1_epi64((long long)(BRAID58_B5 - 1U)));
  propagate |= (unsigned)_mm512_cmpeq_epi64_mask(
      shi, _mm512_set1_epi64((long long)(BRAID58_B5 - 1U))) << 8;
  propagate |= (unsigned)(sum16 == BRAID58_B5 - 1U) << 16;
  if (__builtin_expect(propagate != 0U, 0))
    generate = braid58_prefix17(generate, propagate);
  else
    generate &= 0x1ffffU;
  const unsigned carry_in = (generate << 1) & 0x1ffffU;
  const __m512i one = _mm512_set1_epi64(1);
  slo = _mm512_mask_add_epi64(slo, (__mmask8)carry_in, slo, one);
  shi = _mm512_mask_add_epi64(
      shi, (__mmask8)(carry_in >> 8), shi, one);
  slo = _mm512_mask_sub_epi64(slo, (__mmask8)generate, slo, divisor);
  shi = _mm512_mask_sub_epi64(
      shi, (__mmask8)(generate >> 8), shi, divisor);
  sum16 += (carry_in >> 16) & 1U;
  if ((generate >> 16) & 1U) sum16 -= BRAID58_B5;
  braid58_b5_cells result;
  result.packed[0] = _mm512_cvtepi64_epi32(slo);
  result.packed[1] = _mm512_cvtepi64_epi32(shi);
  result.cell16 = (uint32_t)sum16;
  result.top = (uint32_t)(q16 + ((generate >> 16) & 1U));
  return result;
}

static inline braid58_b5_cells
braid58_make_b5_cells_scalar(const uint8_t input[64]) {
  uint64_t word[8];
  uint32_t chunk[20];
  for (unsigned i = 0; i < 8; ++i) {
    word[i] = braid58_load_be64(input + 56U - 8U * i);
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(word[i]));
#endif
  }
#pragma GCC unroll 20
  for (unsigned i = 0; i < 20; ++i) {
    const unsigned bit = 26U * i;
    const unsigned wi = bit >> 6;
    const unsigned offset = bit & 63U;
    uint64_t value = word[wi] >> offset;
    if (offset > 38U && wi < 7U)
      value |= word[wi + 1U] << (64U - offset);
    chunk[i] = (uint32_t)(value & BRAID58_CHUNK_MASK);
  }

  const uint64_t low52 =
      (uint64_t)chunk[0] | ((uint64_t)chunk[1] << 26);
  __m512i low_even = _mm512_mask_set1_epi64(
      _mm512_setzero_si512(), (__mmask8)1, (long long)low52);
  __m512i low_odd = _mm512_setzero_si512();
  __m512i high_even = _mm512_setzero_si512();
  __m512i high_odd = _mm512_setzero_si512();

#define BRAID58_B5_LO(ACC, ROW) do {                                        \
    (ACC) = _mm512_add_epi64(                                              \
        (ACC), _mm512_mul_epu32(                                           \
            _mm512_set1_epi64((long long)chunk[(ROW)]),                    \
            _mm512_load_si512((const void *)&BRAID58_W5_LO[(ROW) - 2U][0]))); \
    __asm__("" : "+v"(ACC));                                             \
  } while (0)
#define BRAID58_B5_HI(ACC, ROW) do {                                        \
    (ACC) = _mm512_add_epi64(                                              \
        (ACC), _mm512_mul_epu32(                                           \
            _mm512_set1_epi64((long long)chunk[(ROW)]),                    \
            _mm512_load_si512((const void *)&BRAID58_W5_HI[(ROW) - 10U][0]))); \
    __asm__("" : "+v"(ACC));                                             \
  } while (0)

  BRAID58_B5_LO(low_even, 2);  BRAID58_B5_LO(low_odd, 3);
  BRAID58_B5_LO(low_even, 4);  BRAID58_B5_LO(low_odd, 5);
  BRAID58_B5_LO(low_even, 6);  BRAID58_B5_LO(low_odd, 7);
  BRAID58_B5_LO(low_even, 8);  BRAID58_B5_LO(low_odd, 9);
  BRAID58_B5_LO(low_even, 10); BRAID58_B5_HI(high_even, 10);
  BRAID58_B5_LO(low_odd, 11);  BRAID58_B5_HI(high_odd, 11);
  BRAID58_B5_LO(low_even, 12); BRAID58_B5_HI(high_even, 12);
  BRAID58_B5_LO(low_odd, 13);  BRAID58_B5_HI(high_odd, 13);
  BRAID58_B5_LO(low_even, 14); BRAID58_B5_HI(high_even, 14);
  BRAID58_B5_LO(low_odd, 15);  BRAID58_B5_HI(high_odd, 15);
  BRAID58_B5_LO(low_even, 16); BRAID58_B5_HI(high_even, 16);
  BRAID58_B5_LO(low_odd, 17);  BRAID58_B5_HI(high_odd, 17);
  BRAID58_B5_LO(low_even, 18); BRAID58_B5_HI(high_even, 18);
  BRAID58_B5_LO(low_odd, 19);  BRAID58_B5_HI(high_odd, 19);

#undef BRAID58_B5_HI
#undef BRAID58_B5_LO

  const __m512i raw_lo = _mm512_add_epi64(low_even, low_odd);
  const __m512i raw_hi = _mm512_add_epi64(high_even, high_odd);
  const uint64_t raw16 = (uint64_t)chunk[19] * UINT64_C(43110034);
  return braid58_finish_b5_raw(raw_lo, raw_hi, raw16);
}

static inline __attribute__((unused)) braid58_b5_cells
braid58_make_b5_cells(const uint8_t input[64]) {
  const __m512i source = _mm512_loadu_si512((const void *)input);
  const __m512i shifts = _mm512_load_si512(BRAID58_CHUNK_SHIFT);
  const __m512i mask = _mm512_set1_epi64(BRAID58_CHUNK_MASK);
  __m512i chunk0 = _mm512_permutexvar_epi8(
      _mm512_load_si512(BRAID58_CHUNK_INDEX[0]), source);
  __m512i chunk1 = _mm512_permutexvar_epi8(
      _mm512_load_si512(BRAID58_CHUNK_INDEX[1]), source);
  __m512i chunk2 = _mm512_maskz_permutexvar_epi8(
      (__mmask64)UINT64_C(0x073fffff),
      _mm512_load_si512(BRAID58_CHUNK_INDEX[2]), source);
  chunk0 = _mm512_and_si512(_mm512_srlv_epi64(chunk0, shifts), mask);
  chunk1 = _mm512_and_si512(_mm512_srlv_epi64(chunk1, shifts), mask);
  chunk2 = _mm512_and_si512(_mm512_srlv_epi64(chunk2, shifts), mask);

  const uint64_t low52 = braid58_load_be64(input + 56) &
      ((UINT64_C(1) << 52) - 1U);
  __m512i low_even = _mm512_mask_set1_epi64(
      _mm512_setzero_si512(), (__mmask8)1, (long long)low52);
  __m512i low_odd = _mm512_setzero_si512();
  __m512i high_even = _mm512_setzero_si512();
  __m512i high_odd = _mm512_setzero_si512();

#define BRAID58_CHUNK(ROW) _mm512_permutexvar_epi64(                        \
    _mm512_set1_epi64((ROW) & 7U),                                         \
    (ROW) < 8U ? chunk0 : ((ROW) < 16U ? chunk1 : chunk2))
#define BRAID58_B5_VLO(ACC, ROW) do {                                       \
    (ACC) = _mm512_add_epi64(                                              \
        (ACC), _mm512_mul_epu32(                                           \
            BRAID58_CHUNK(ROW),                                            \
            _mm512_load_si512((const void *)&BRAID58_W5_LO[(ROW) - 2U][0]))); \
    __asm__("" : "+v"(ACC));                                             \
  } while (0)
#define BRAID58_B5_VHI(ACC, ROW) do {                                       \
    (ACC) = _mm512_add_epi64(                                              \
        (ACC), _mm512_mul_epu32(                                           \
            BRAID58_CHUNK(ROW),                                            \
            _mm512_load_si512((const void *)&BRAID58_W5_HI[(ROW) - 10U][0]))); \
    __asm__("" : "+v"(ACC));                                             \
  } while (0)

  BRAID58_B5_VLO(low_even, 2);  BRAID58_B5_VLO(low_odd, 3);
  BRAID58_B5_VLO(low_even, 4);  BRAID58_B5_VLO(low_odd, 5);
  BRAID58_B5_VLO(low_even, 6);  BRAID58_B5_VLO(low_odd, 7);
  BRAID58_B5_VLO(low_even, 8);  BRAID58_B5_VLO(low_odd, 9);
  BRAID58_B5_VLO(low_even, 10); BRAID58_B5_VHI(high_even, 10);
  BRAID58_B5_VLO(low_odd, 11);  BRAID58_B5_VHI(high_odd, 11);
  BRAID58_B5_VLO(low_even, 12); BRAID58_B5_VHI(high_even, 12);
  BRAID58_B5_VLO(low_odd, 13);  BRAID58_B5_VHI(high_odd, 13);
  BRAID58_B5_VLO(low_even, 14); BRAID58_B5_VHI(high_even, 14);
  BRAID58_B5_VLO(low_odd, 15);  BRAID58_B5_VHI(high_odd, 15);
  BRAID58_B5_VLO(low_even, 16); BRAID58_B5_VHI(high_even, 16);
  BRAID58_B5_VLO(low_odd, 17);  BRAID58_B5_VHI(high_odd, 17);
  BRAID58_B5_VLO(low_even, 18); BRAID58_B5_VHI(high_even, 18);
  BRAID58_B5_VLO(low_odd, 19);  BRAID58_B5_VHI(high_odd, 19);

#undef BRAID58_B5_VHI
#undef BRAID58_B5_VLO
#undef BRAID58_CHUNK

  const __m512i raw_lo = _mm512_add_epi64(low_even, low_odd);
  const __m512i raw_hi = _mm512_add_epi64(high_even, high_odd);
  const uint64_t chunk19 = (uint64_t)_mm_extract_epi64(
      _mm512_extracti64x2_epi64(chunk2, 1), 1);
  return braid58_finish_b5_raw(
      raw_lo, raw_hi, chunk19 * UINT64_C(43110034));
}

typedef struct {
  __m256i first32;
  __m128i last8;
} braid58_digit40;

static inline __m256i
braid58_div195112_u32(__m256i x) {
  const __m256i magic = _mm256_set1_epi32(721316415);
  const __m256i even = _mm256_mul_epu32(x, magic);
  const __m256i odd = _mm256_mul_epu32(_mm256_srli_epi64(x, 32), magic);
  const __m256i high = _mm256_blend_epi32(
      _mm256_shuffle_epi32(even, 0xf5), odd, 0xaa);
  return _mm256_srli_epi32(high, 15);
}

static inline __m256i
braid58_map58(__m256i digits) {
  const __m256i index = _mm256_and_si256(digits, _mm256_set1_epi8(15));
  const __m256i bit4 = _mm256_slli_epi16(digits, 3);
  const __m256i bit5 = _mm256_slli_epi16(digits, 2);
  const __m256i low = _mm256_blendv_epi8(
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)BRAID58_MAP0), index),
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)BRAID58_MAP1), index), bit4);
  const __m256i high = _mm256_blendv_epi8(
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)BRAID58_MAP2), index),
      _mm256_shuffle_epi8(
          _mm256_load_si256((const __m256i *)BRAID58_MAP3), index), bit4);
  return _mm256_blendv_epi8(low, high, bit5);
}

/* Return lanes 7..0 as eight consecutive five-digit records. */
static inline braid58_digit40
braid58_split8_b5(__m256i cell) {
  const __m256i zero = _mm256_setzero_si256();
  const __m256i q3 = braid58_div195112_u32(cell);
  const __m256i r3 = _mm256_sub_epi32(
      cell, _mm256_mullo_epi32(q3, _mm256_set1_epi32(195112)));
  const __m256i quarter = _mm256_srli_epi32(r3, 2);
  const __m256i pair = _mm256_packus_epi32(q3, quarter);
  const __m256i magic = _mm256_setr_epi16(
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899,
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899);
  const __m256i first_high = _mm256_mulhi_epu16(pair, magic);
  const __m256i high = _mm256_blend_epi16(
      first_high, _mm256_srli_epi16(first_high, 9), 0xf0);
  const __m256i coefficient = _mm256_setr_epi16(
      58,58,58,58,841,841,841,841,58,58,58,58,841,841,841,841);
  const __m256i rem = _mm256_sub_epi16(
      pair, _mm256_mullo_epi16(high, coefficient));
  const __m256i scale = _mm256_setr_epi16(
      1,1,1,1,4,4,4,4,1,1,1,1,4,4,4,4);
  __m256i pair58 = _mm256_mullo_epi16(rem, scale);
  pair58 = _mm256_add_epi16(
      pair58, _mm256_packus_epi32(
          zero, _mm256_and_si256(r3, _mm256_set1_epi32(3))));
  const __m256i middle = _mm256_mulhi_epu16(
      pair58, _mm256_set1_epi16(1130));
  const __m256i low = _mm256_sub_epi16(
      pair58, _mm256_mullo_epi16(middle, _mm256_set1_epi16(58)));
  const __m256i bytes = _mm256_packus_epi16(low, middle);
  const __m256i tops = _mm256_packus_epi16(high, zero);
  const __m256i fields = _mm256_or_si256(
      _mm256_shuffle_epi8(
          bytes, _mm256_load_si256((const __m256i *)BRAID58_FIELDS_A)),
      _mm256_shuffle_epi8(
          tops, _mm256_load_si256((const __m256i *)BRAID58_FIELDS_B)));
  const __m256i head = _mm256_or_si256(
      _mm256_shuffle_epi8(
          fields, _mm256_load_si256((const __m256i *)BRAID58_HEAD_A)),
      _mm256_shuffle_epi8(
          tops, _mm256_load_si256((const __m256i *)BRAID58_HEAD_B)));
  const __m256i tail = _mm256_shuffle_epi8(
      fields, _mm256_load_si256((const __m256i *)BRAID58_TAIL_A));
  const __m128i head_low = _mm256_castsi256_si128(head);
  const __m128i head_high = _mm256_extracti128_si256(head, 1);
  const uint32_t tail_low =
      (uint32_t)_mm_cvtsi128_si32(_mm256_castsi256_si128(tail));
  const uint32_t tail_high = (uint32_t)_mm_cvtsi128_si32(
      _mm256_extracti128_si256(tail, 1));
  const __m128i second = _mm_alignr_epi8(
      head_low, _mm_slli_si128(_mm_cvtsi32_si128((int)tail_high), 12), 12);
  braid58_digit40 result;
  result.first32 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(head_high), second, 1);
  result.last8 = _mm_cvtsi64_si128(
      (long long)((uint64_t)(uint32_t)_mm_extract_epi32(head_low, 3) |
                  ((uint64_t)tail_low << 32)));
  return result;
}

static inline void
braid58_split_scalar5(uint32_t value, uint8_t output[5]) {
  for (unsigned i = 5; i--;) {
    output[i] = (uint8_t)(value % 58U);
    value /= 58U;
  }
}

static inline size_t
braid58_emit_b5(const uint8_t input[64], braid58_b5_cells cells,
                char output[89]) {
  const __m256i zero = _mm256_setzero_si256();
  _Alignas(16) uint8_t top_digit[16] = {0};
  braid58_split_scalar5(cells.top, top_digit);
  braid58_split_scalar5(cells.cell16, top_digit + 5);
  const __m128i top10 = _mm_load_si128((const __m128i *)top_digit);
  const braid58_digit40 block_a = braid58_split8_b5(cells.packed[1]);
  const braid58_digit40 block_b = braid58_split8_b5(cells.packed[0]);
  const __m128i alo = _mm256_castsi256_si128(block_a.first32);
  const __m128i ahi = _mm256_extracti128_si256(block_a.first32, 1);
  const __m128i blo = _mm256_castsi256_si128(block_b.first32);
  const __m128i bhi = _mm256_extracti128_si256(block_b.first32, 1);
  const __m128i d0lo = _mm_alignr_epi8(alo, _mm_slli_si128(top10, 6), 6);
  const __m128i d0hi = _mm_alignr_epi8(ahi, alo, 6);
  const __m128i d1lo = _mm_alignr_epi8(block_a.last8, ahi, 6);
  const __m128i a_tail2 =
      _mm_slli_si128(_mm_srli_si128(block_a.last8, 6), 14);
  const __m128i d1hi = _mm_alignr_epi8(blo, a_tail2, 14);
  const __m128i d2lo = _mm_alignr_epi8(bhi, blo, 14);
  const __m128i d2hi = _mm_alignr_epi8(block_b.last8, bhi, 14);
  const __m256i d0 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(d0lo), d0hi, 1);
  const __m256i d1 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(d1lo), d1hi, 1);
  const __m256i d2 = _mm256_inserti128_si256(
      _mm256_castsi128_si256(d2lo), d2hi, 1);
  const __m256i ascii0 = braid58_map58(d0);
  const __m256i ascii1 = braid58_map58(d1);
  const __m256i ascii2 = braid58_map58(d2);

  unsigned skip;
  if (__builtin_expect(input[0] != 0, 1)) {
    skip = top_digit[2] != 0 ? 2U : 3U;
    const __m256i cross01 =
        _mm256_permute2x128_si256(ascii0, ascii1, 0x21);
    const __m256i cross12 =
        _mm256_permute2x128_si256(ascii1, ascii2, 0x21);
    const __m256i first = skip == 2U
        ? _mm256_alignr_epi8(cross01, ascii0, 2)
        : _mm256_alignr_epi8(cross01, ascii0, 3);
    const __m256i second = skip == 2U
        ? _mm256_alignr_epi8(cross12, ascii1, 2)
        : _mm256_alignr_epi8(cross12, ascii1, 3);
    const __m128i tail_low = _mm256_extracti128_si256(
        _mm256_alignr_epi8(cross12, ascii1, 10), 1);
    const __m128i tail_high = _mm_alignr_epi8(
        _mm256_extracti128_si256(ascii2, 1),
        _mm256_castsi256_si128(ascii2), 10);
    const __m256i tail = _mm256_inserti128_si256(
        _mm256_castsi128_si256(tail_low), tail_high, 1);
    const size_t length = 90U - skip;
    _mm256_storeu_si256((__m256i *)output, first);
    _mm256_storeu_si256((__m256i *)(output + 32), second);
    _mm256_storeu_si256((__m256i *)(output + length - 32U), tail);
    output[length] = '\0';
    return length;
  }

  uint64_t nonzero = (uint32_t)~(uint32_t)_mm256_movemask_epi8(
      _mm256_cmpeq_epi8(d0, zero));
  nonzero |= (uint64_t)(uint32_t)~(uint32_t)_mm256_movemask_epi8(
      _mm256_cmpeq_epi8(d1, zero)) << 32;
  const uint32_t nonzero2 =
      ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(d2, zero));
  const unsigned first_digit = nonzero
      ? (unsigned)__builtin_ctzll(nonzero)
      : (nonzero2 ? 64U + (unsigned)__builtin_ctz(nonzero2) : 90U);
  const __m256i input0 = _mm256_loadu_si256((const __m256i *)input);
  const __m256i input1 = _mm256_loadu_si256((const __m256i *)(input + 32));
  const uint32_t bytes0 =
      ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(input0, zero));
  const uint32_t bytes1 =
      ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(input1, zero));
  const unsigned leading = bytes0
      ? (unsigned)__builtin_ctz(bytes0)
      : (bytes1 ? 32U + (unsigned)__builtin_ctz(bytes1) : 64U);
  skip = first_digit - leading;
  _Alignas(32) uint8_t ascii[96];
  _mm256_store_si256((__m256i *)ascii, ascii0);
  _mm256_store_si256((__m256i *)(ascii + 32), ascii1);
  _mm256_store_si256((__m256i *)(ascii + 64), ascii2);
  const size_t length = 90U - skip;
  _mm256_storeu_si256(
      (__m256i *)output,
      _mm256_loadu_si256((const __m256i *)(ascii + skip)));
  _mm256_storeu_si256(
      (__m256i *)(output + 32),
      _mm256_loadu_si256((const __m256i *)(ascii + skip + 32)));
  _mm256_storeu_si256(
      (__m256i *)(output + length - 32U),
      _mm256_loadu_si256((const __m256i *)(ascii + 58)));
  output[length] = '\0';
  return length;
}

/* Convert eight radix-58^6 cells to 48 big-endian digit values. */
static inline __m512i
braid58_split_cells(__m512i cells) {
  const __m512i q3 = _mm512_srli_epi64(
      _mm512_madd52hi_epu64(
          _mm512_setzero_si512(), cells,
          _mm512_set1_epi64(INT64_C(94544385141404))),
      12);
  const __m512i r3 = _mm512_sub_epi64(
      cells,
      _mm512_mullo_epi64(q3, _mm512_set1_epi64((long long)BRAID58_B3)));
  const __m256i q3d = _mm512_cvtepi64_epi32(q3);
  const __m256i r3d = _mm512_cvtepi64_epi32(r3);
  __m512i v3 = _mm512_castsi256_si512(q3d);
  v3 = _mm512_inserti64x4(v3, r3d, 1);

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
  const __m256i middle16 = _mm256_mulhi_epu16(
      pair16, _mm256_set1_epi16(1130));
  const __m256i low16 = _mm256_sub_epi16(
      pair16, _mm256_mullo_epi16(middle16, _mm256_set1_epi16(58)));
  const __m128i lead8 = _mm256_cvtepi16_epi8(lead16);
  const __m128i middle8 = _mm256_cvtepi16_epi8(middle16);
  const __m128i low8 = _mm256_cvtepi16_epi8(low16);
  __m512i fields = _mm512_castsi128_si512(lead8);
  fields = _mm512_inserti32x4(fields, middle8, 1);
  fields = _mm512_inserti32x4(fields, low8, 2);
  return _mm512_permutexvar_epi8(
      _mm512_load_si512(BRAID58_ORDER6), fields);
}

static inline __attribute__((unused)) size_t
braid58_emit(const uint8_t input[64], braid58_cells15 cells,
             char output[89]) {
  const __m512i zero = _mm512_setzero_si512();
  const __m512i digits_hi = braid58_split_cells(cells.hi);
  const __m512i digits_lo = braid58_split_cells(cells.lo);
  const __m512i alphabet = _mm512_load_si512(BRAID58_ALPHABET);
  const __m512i ascii_hi = _mm512_permutexvar_epi8(digits_hi, alphabet);
  const __m512i ascii_lo = _mm512_permutexvar_epi8(digits_lo, alphabet);

  if (__builtin_expect(input[0] != 0, 1)) {
    const uint64_t nonzero =
        (uint64_t)_mm512_cmpneq_epi8_mask(digits_hi, zero) &
        ((UINT64_C(1) << 48) - 1U);
    const unsigned skip = (unsigned)__builtin_ctzll(nonzero);
    const size_t length = 96U - skip;
    const __m512i join_index = _mm512_load_si512(
        skip == 8U ? (const void *)BRAID58_JOIN_SKIP8
                   : (const void *)BRAID58_JOIN_SKIP9);
    const __m512i first = _mm512_permutex2var_epi8(
        ascii_hi, join_index, ascii_lo);
    const __m256i tail = _mm512_castsi512_si256(
        _mm512_alignr_epi32(ascii_lo, ascii_lo, 4));
    _mm512_storeu_si512((void *)output, first);
    _mm256_storeu_si256((__m256i *)(void *)(output + length - 32U), tail);
    output[length] = '\0';
    return length;
  }

  const uint64_t nz_hi =
      (uint64_t)_mm512_cmpneq_epi8_mask(digits_hi, zero) &
      ((UINT64_C(1) << 48) - 1U);
  const uint64_t nz_lo =
      (uint64_t)_mm512_cmpneq_epi8_mask(digits_lo, zero) &
      ((UINT64_C(1) << 48) - 1U);
  const unsigned first_digit = nz_hi
      ? (unsigned)__builtin_ctzll(nz_hi)
      : (nz_lo ? 48U + (unsigned)__builtin_ctzll(nz_lo) : 96U);
  const __m512i input_bytes =
      _mm512_loadu_si512((const void *)input);
  const uint64_t nonzero_bytes =
      ~(uint64_t)_mm512_cmpeq_epi8_mask(input_bytes, zero);
  const unsigned leading_zeroes = nonzero_bytes
      ? (unsigned)__builtin_ctzll(nonzero_bytes) : 64U;
  const unsigned skip = first_digit - leading_zeroes;
  const size_t length = 96U - skip;

  _Alignas(64) uint8_t ascii[112];
  _mm512_mask_storeu_epi8(
      (void *)ascii, (__mmask64)((UINT64_C(1) << 48) - 1U), ascii_hi);
  _mm512_mask_storeu_epi8(
      (void *)(ascii + 48), (__mmask64)((UINT64_C(1) << 48) - 1U), ascii_lo);
  _mm256_storeu_si256(
      (__m256i *)(void *)output,
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii + skip)));
  _mm256_storeu_si256(
      (__m256i *)(void *)(output + 32),
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii + skip + 32)));
  _mm256_storeu_si256(
      (__m256i *)(void *)(output + length - 32U),
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii + 64)));
  output[length] = '\0';
  return length;
}

size_t
braid58_encode_64_avx512(const uint8_t input[64], char output[89]) {
  return braid58_emit_b5(input, braid58_make_b5_cells_scalar(input), output);
}
