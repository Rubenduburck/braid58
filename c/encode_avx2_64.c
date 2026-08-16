/* AVX2 Bitcoin Base58 encoder for 64-byte inputs; radix 2^26 to radix 58^5. */
#include "braid58_internal.h"

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

#define B58_B5 UINT64_C(656356768)
#define B58_B3 UINT32_C(195112)
#define B58_B2 UINT32_C(3364)
#define MASK26 UINT64_C(0x3ffffff)

/* Little-endian radix-58^5 expansion of 2^(26*i), padded to five YMMs. */
_Alignas(32) static const uint64_t W[20][20] = {
  {1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {67108864,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {443814048,6861511,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {437973984,506963877,701551,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {192414240,584944171,527870455,71729, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {85356032,237320048,230466711,641497958, 7333,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {59086336,425716400,469620603,369434287, 563670112,749,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {249488768,563819044,543108756,589143927, 28959436,439056932,76,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {147129216,329522580,449746271,218521078, 590921969,502289454,550667440,7, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {576074656,432829647,553198108,378465102, 185459504,598754100,313589673,526064760, 0,0,0,0, 0,0,0,0, 0,0,0,0},
  {184037504,172044172,216479247,655243859, 487688611,347228068,495642092,618120192, 53787223,0,0,0, 0,0,0,0, 0,0,0,0},
  {11240928,599793205,368209896,143682386, 101050947,216709650,476932781,529427649, 237736760,5499447,0,0, 0,0,0,0, 0,0,0,0},
  {635074496,173439274,448067151,363842761, 469972204,648159646,470224919,553330405, 147241268,130740298,562288,0, 0,0,0,0, 0,0,0,0},
  {604481920,314192324,31450010,106032878, 54664182,329747909,554128193,640039347, 435587593,205535280,571695987,57490, 0,0,0,0, 0,0,0,0},
  {356764352,328895570,216399954,459733446, 334397277,193013911,250503419,441894667, 38393073,171253552,345880098,81961821, 5878,0,0,0, 0,0,0,0},
  {500650976,245129989,205412112,298516231, 409710268,156613671,479004433,375386322, 194031727,230544232,507785892,502831086, 3865168,601,0,0, 0,0,0,0},
  {308625792,104784612,644355351,184514320, 45134568,144614682,467589845,453637228, 212906911,527697587,239296485,517024870, 141201404,295059608,61,0, 0,0,0,0},
  {113587872,276429623,155974033,608422969, 645780644,416442536,633095654,514800901, 608440467,323200398,337103515,639109435, 547702080,400446185,185668315,6, 0,0,0,0},
  {141445696,172260198,484063438,78047805, 442478329,289649177,191490987,298973999, 163380196,293664278,348652084,170553025, 507831179,427843872,341939960,421636746, 0,0,0,0},
  {174092320,246601307,71321328,139080596, 505046486,584509323,421142136,351377042, 72896988,529815445,60242290,244603179, 460450843,145566876,267323783,495067909, 43110034,0,0,0},
};

_Alignas(64) static const uint32_t EXPAND4[16] = {
  0x00000000,0x00000001,0x00000100,0x00000101,
  0x00010000,0x00010001,0x00010100,0x00010101,
  0x01000000,0x01000001,0x01000100,0x01000101,
  0x01010000,0x01010001,0x01010100,0x01010101,
};

_Alignas(32) static const uint8_t MAP0[] = "123456789ABCDEFG123456789ABCDEFG";
_Alignas(32) static const uint8_t MAP1[] = "HJKLMNPQRSTUVWXYHJKLMNPQRSTUVWXY";
_Alignas(32) static const uint8_t MAP2[] = "ZabcdefghijkmnopZabcdefghijkmnop";
_Alignas(32) static const uint8_t MAP3[] = "qrstuvwxyz\0\0\0\0\0\0qrstuvwxyz\0\0\0\0\0\0";

_Alignas(32) static const uint8_t FIELDS_A[32] = {
   4,5,6,7,12,13,14,15, 0x80,0x80,0x80,0x80,0,1,2,3,
   4,5,6,7,12,13,14,15, 0x80,0x80,0x80,0x80,0,1,2,3,
};
_Alignas(32) static const uint8_t FIELDS_B[32] = {
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,4,5,6,7,0x80,0x80,0x80,0x80,
  0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,4,5,6,7,0x80,0x80,0x80,0x80,
};
_Alignas(32) static const uint8_t HEAD_A[32] = {
  0x80,15,11,7,3,0x80,14,10,6,2,0x80,13,9,5,1,0x80,
  0x80,15,11,7,3,0x80,14,10,6,2,0x80,13,9,5,1,0x80,
};
_Alignas(32) static const uint8_t HEAD_B[32] = {
  3,0x80,0x80,0x80,0x80,2,0x80,0x80,0x80,0x80,1,0x80,0x80,0x80,0x80,0,
  3,0x80,0x80,0x80,0x80,2,0x80,0x80,0x80,0x80,1,0x80,0x80,0x80,0x80,0,
};
_Alignas(32) static const uint8_t TAIL_A[32] = {
  12,8,4,0,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
  12,8,4,0,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
};

static inline uint64_t load_be64(const void *p) {
  uint64_t x; memcpy(&x,p,8); return __builtin_bswap64(x);
}

static inline void div_b5(__m256i x, __m256i *qout, __m256i *rout) {
  const __m256i magic=_mm256_set1_epi64x((long long)UINT64_C(3513093979));
  const __m256i divisor=_mm256_set1_epi64x((long long)B58_B5);
  const __m256i p0=_mm256_mul_epu32(x,magic);
  const __m256i p1=_mm256_mul_epu32(_mm256_srli_epi64(x,32),magic);
  __m256i q=_mm256_srli_epi64(_mm256_add_epi64(p1,_mm256_srli_epi64(p0,32)),29);
  __m256i product=_mm256_mul_epu32(q,divisor);
  const __m256i high=_mm256_cmpgt_epi64(product,x);
  q=_mm256_add_epi64(q,high);
  product=_mm256_sub_epi64(product,_mm256_and_si256(high,divisor));
  *qout=q; *rout=_mm256_sub_epi64(x,product);
}

static inline __m256i div195112_u32(__m256i x) {
  const __m256i m=_mm256_set1_epi32(721316415);
  const __m256i e=_mm256_mul_epu32(x,m);
  const __m256i o=_mm256_mul_epu32(_mm256_srli_epi64(x,32),m);
  const __m256i h=_mm256_blend_epi32(_mm256_shuffle_epi32(e,0xf5),o,0xaa);
  return _mm256_srli_epi32(h,15);
}

static inline __m256i map58(__m256i d) {
  const __m256i idx=_mm256_and_si256(d,_mm256_set1_epi8(15));
  const __m256i b4=_mm256_slli_epi16(d,3);
  const __m256i b5=_mm256_slli_epi16(d,2);
  const __m256i lo=_mm256_blendv_epi8(
      _mm256_shuffle_epi8(_mm256_load_si256((const __m256i *)(const void *)MAP0),idx),
      _mm256_shuffle_epi8(_mm256_load_si256((const __m256i *)(const void *)MAP1),idx),b4);
  const __m256i hi=_mm256_blendv_epi8(
      _mm256_shuffle_epi8(_mm256_load_si256((const __m256i *)(const void *)MAP2),idx),
      _mm256_shuffle_epi8(_mm256_load_si256((const __m256i *)(const void *)MAP3),idx),b4);
  return _mm256_blendv_epi8(lo,hi,b5);
}

typedef struct { __m256i first32; __m128i last8; } digit40;

/* Return lanes 7..0 as eight consecutive five-digit records. */
static inline digit40 split8(__m256i cell) {
  const __m256i zero=_mm256_setzero_si256();
  const __m256i q3=div195112_u32(cell);
  const __m256i r3=_mm256_sub_epi32(
      cell,_mm256_mullo_epi32(q3,_mm256_set1_epi32((int)B58_B3)));
  const __m256i quarter=_mm256_srli_epi32(r3,2);
  const __m256i pair=_mm256_packus_epi32(q3,quarter);
  const __m256i magic=_mm256_setr_epi16(
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899,
      1130,1130,1130,1130,(short)39899,(short)39899,(short)39899,(short)39899);
  const __m256i fh=_mm256_mulhi_epu16(pair,magic);
  const __m256i high=_mm256_blend_epi16(fh,_mm256_srli_epi16(fh,9),0xf0);
  const __m256i coeff=_mm256_setr_epi16(
      58,58,58,58,841,841,841,841,58,58,58,58,841,841,841,841);
  const __m256i rem=_mm256_sub_epi16(pair,_mm256_mullo_epi16(high,coeff));
  const __m256i scale=_mm256_setr_epi16(
      1,1,1,1,4,4,4,4,1,1,1,1,4,4,4,4);
  __m256i pair58=_mm256_mullo_epi16(rem,scale);
  pair58=_mm256_add_epi16(pair58,_mm256_packus_epi32(
      zero,_mm256_and_si256(r3,_mm256_set1_epi32(3))));
  const __m256i middle=_mm256_mulhi_epu16(pair58,_mm256_set1_epi16(1130));
  const __m256i low=_mm256_sub_epi16(
      pair58,_mm256_mullo_epi16(middle,_mm256_set1_epi16(58)));
  const __m256i bytes=_mm256_packus_epi16(low,middle);
  const __m256i tops=_mm256_packus_epi16(high,zero);
  const __m256i fields=_mm256_or_si256(
      _mm256_shuffle_epi8(bytes,_mm256_load_si256((const __m256i *)(const void *)FIELDS_A)),
      _mm256_shuffle_epi8(tops,_mm256_load_si256((const __m256i *)(const void *)FIELDS_B)));
  const __m256i head=_mm256_or_si256(
      _mm256_shuffle_epi8(fields,_mm256_load_si256((const __m256i *)(const void *)HEAD_A)),
      _mm256_shuffle_epi8(tops,_mm256_load_si256((const __m256i *)(const void *)HEAD_B)));
  const __m256i tail=_mm256_shuffle_epi8(
      fields,_mm256_load_si256((const __m256i *)(const void *)TAIL_A));
  const __m128i hlo=_mm256_castsi256_si128(head);
  const __m128i hhi=_mm256_extracti128_si256(head,1);
  const uint32_t tlo=(uint32_t)_mm_cvtsi128_si32(_mm256_castsi256_si128(tail));
  const uint32_t thi=(uint32_t)_mm_cvtsi128_si32(_mm256_extracti128_si256(tail,1));
  const __m128i second=_mm_alignr_epi8(hlo,_mm_slli_si128(
      _mm_cvtsi32_si128((int)thi),12),12);
  digit40 result;
  result.first32=_mm256_inserti128_si256(_mm256_castsi128_si256(hhi),second,1);
  const uint64_t packed_tail=(uint64_t)(uint32_t)_mm_extract_epi32(hlo,3)
                            |((uint64_t)tlo<<32);
  result.last8=_mm_cvtsi64_si128((long long)packed_tail);
  return result;
}

static inline void split_scalar(uint32_t x, uint8_t out[5]) {
  for(unsigned i=5;i--;) { out[i]=(uint8_t)(x%58U); x/=58U; }
}

typedef struct { __m256i c[5]; } raw64;

static __attribute__((always_inline)) inline raw64
make_raw64(const uint8_t input[64]) {
  uint64_t word[8];
  uint32_t a[20];
  for(unsigned i=0;i<8;i++) {
    word[i]=load_be64(input+56U-8U*i);
    __asm__("" : "+r"(word[i]));
  }
#pragma GCC unroll 20
  for(unsigned i=0;i<20;i++) {
    const unsigned bit=26U*i, wi=bit>>6, off=bit&63U;
    uint64_t x=word[wi]>>off;
    if(off>38U && wi<7U) x|=word[wi+1]<<(64U-off);
    a[i]=(uint32_t)(x&MASK26);
  }

  __m256i c[5];
  c[0]=_mm256_setr_epi64x((long long)((uint64_t)a[0]+((uint64_t)a[1]<<26)),0,0,0);
  c[1]=c[2]=c[3]=c[4]=_mm256_setzero_si256();
#define ADD(R,G) do {                                                        \
    const __m256i av=_mm256_set1_epi64x((long long)a[(R)]);                 \
    c[(G)]=_mm256_add_epi64(c[(G)],_mm256_mul_epu32(                        \
        av,_mm256_load_si256((const __m256i *)(const void *)&W[(R)][4*(G)]))); \
    __asm__("" : "+x"(c[(G)]));                                           \
  } while(0)
  ADD(2,0); ADD(3,0); ADD(4,0);
  ADD(5,0); ADD(5,1); ADD(6,0); ADD(6,1); ADD(7,0); ADD(7,1);
  ADD(8,0); ADD(8,1); ADD(9,0); ADD(9,1);
  ADD(10,0); ADD(10,1); ADD(10,2);
  ADD(11,0); ADD(11,1); ADD(11,2);
  ADD(12,0); ADD(12,1); ADD(12,2);
  ADD(13,0); ADD(13,1); ADD(13,2);
  ADD(14,0); ADD(14,1); ADD(14,2); ADD(14,3);
  ADD(15,0); ADD(15,1); ADD(15,2); ADD(15,3);
  ADD(16,0); ADD(16,1); ADD(16,2); ADD(16,3);
  ADD(17,0); ADD(17,1); ADD(17,2); ADD(17,3);
  ADD(18,0); ADD(18,1); ADD(18,2); ADD(18,3);
  ADD(19,0); ADD(19,1); ADD(19,2); ADD(19,3); ADD(19,4);
#undef ADD

  raw64 raw;
  for(unsigned i=0;i<5;i++) raw.c[i]=c[i];
  return raw;
}

typedef struct {
  __m256i packed[2];
  uint32_t top;
  uint32_t cell16;
} cells64;

static __attribute__((always_inline)) inline cells64
finish_raw64(raw64 raw) {

  __m256i q[5],r[5],s[5],initial_gen[5],peq[5];
  for(unsigned i=0;i<5;i++) div_b5(raw.c[i],&q[i],&r[i]);
  const __m256i zero=_mm256_setzero_si256();
  __m256i shifted=_mm256_permute4x64_epi64(q[0],0x90);
  shifted=_mm256_blend_epi32(zero,shifted,0xfc);
  s[0]=_mm256_add_epi64(r[0],shifted);
  for(unsigned i=1;i<5;i++) {
    const __m256i cross=_mm256_permute2x128_si256(q[i-1],q[i],0x21);
    shifted=_mm256_alignr_epi8(q[i],cross,8);
    s[i]=_mm256_add_epi64(r[i],shifted);
  }

  const __m256i limit=_mm256_set1_epi64x((long long)(B58_B5-1));
  unsigned g=0;
  __m256i any_p=zero;
  for(unsigned i=0;i<5;i++) {
    initial_gen[i]=_mm256_cmpgt_epi64(s[i],limit);
    peq[i]=_mm256_cmpeq_epi64(s[i],limit);
    g|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(initial_gen[i]))<<(4*i);
    any_p=_mm256_or_si256(any_p,peq[i]);
  }
  g&=0x1ffffU;
  const unsigned had_p=(unsigned)!_mm256_testz_si256(any_p,any_p);
  if(__builtin_expect(had_p!=0U,0)) {
    unsigned p=0;
    for(unsigned i=0;i<5;i++)
      p|=(unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(peq[i]))<<(4*i);
    p&=0x1ffffU;
    g|=p&(g<<1); p&=p<<1;
    g|=p&(g<<2); p&=p<<2;
    g|=p&(g<<4); p&=p<<4;
    g|=p&(g<<8); p&=p<<8;
    g|=p&(g<<16); g&=0x1ffffU;
  }
  const unsigned carry_in=(g<<1)&0x1ffffU;
  for(unsigned i=0;i<5;i++) {
    const __m256i cv=_mm256_cvtepu8_epi64(
        _mm_cvtsi32_si128((int)EXPAND4[(carry_in>>(4*i))&15U]));
    s[i]=_mm256_add_epi64(s[i],cv);
  }
  const __m256i divisor=_mm256_set1_epi64x((long long)B58_B5);
  for(unsigned i=0;i<5;i++) {
    const __m256i gen=had_p ? _mm256_cmpgt_epi64(s[i],limit)
                            : initial_gen[i];
    s[i]=_mm256_sub_epi64(s[i],_mm256_and_si256(gen,divisor));
  }
  cells64 result;
  result.top=(uint32_t)(uint64_t)_mm256_extract_epi64(q[4],0)
             +((g>>16)&1U);
  const __m256i index=_mm256_setr_epi32(0,2,4,6,0,0,0,0);
  for(unsigned k=0;k<2;k++) {
    const __m256i lo=_mm256_permutevar8x32_epi32(s[2*k],index);
    const __m256i hi=_mm256_permutevar8x32_epi32(s[2*k+1],index);
    result.packed[k]=_mm256_permute2x128_si256(lo,hi,0x20);
  }
  result.cell16=(uint32_t)(uint64_t)_mm256_extract_epi64(s[4],0);
  return result;
}

static __attribute__((always_inline)) inline size_t
emit64(const uint8_t input[64], cells64 cells, char output[89]) {
  const __m256i zero=_mm256_setzero_si256();
  _Alignas(16) uint8_t top_digit[16]={0};
  split_scalar(cells.top,top_digit);
  split_scalar(cells.cell16,top_digit+5);
  const __m128i top10=_mm_load_si128((const __m128i *)(const void *)top_digit);
  const digit40 block_a=split8(cells.packed[1]);
  const digit40 block_b=split8(cells.packed[0]);
  const __m128i alo=_mm256_castsi256_si128(block_a.first32);
  const __m128i ahi=_mm256_extracti128_si256(block_a.first32,1);
  const __m128i blo=_mm256_castsi256_si128(block_b.first32);
  const __m128i bhi=_mm256_extracti128_si256(block_b.first32,1);
  const __m128i d0lo=_mm_alignr_epi8(alo,_mm_slli_si128(top10,6),6);
  const __m128i d0hi=_mm_alignr_epi8(ahi,alo,6);
  const __m128i d1lo=_mm_alignr_epi8(block_a.last8,ahi,6);
  const __m128i a_tail2=_mm_slli_si128(_mm_srli_si128(block_a.last8,6),14);
  const __m128i d1hi=_mm_alignr_epi8(blo,a_tail2,14);
  const __m128i d2lo=_mm_alignr_epi8(bhi,blo,14);
  const __m128i d2hi=_mm_alignr_epi8(block_b.last8,bhi,14);
  const __m256i d0=_mm256_inserti128_si256(_mm256_castsi128_si256(d0lo),d0hi,1);
  const __m256i d1=_mm256_inserti128_si256(_mm256_castsi128_si256(d1lo),d1hi,1);
  const __m256i d2=_mm256_inserti128_si256(_mm256_castsi128_si256(d2lo),d2hi,1);
  const __m256i ascii0=map58(d0),ascii1=map58(d1),ascii2=map58(d2);

  unsigned skip;
  if(__builtin_expect(input[0]!=0,1)) {
    /* 2^504 > 58^86, hence a full-width value has 87 or 88 digits. */
    skip=top_digit[2]!=0 ? 2U : 3U;
    const __m256i cross01=_mm256_permute2x128_si256(ascii0,ascii1,0x21);
    const __m256i cross12=_mm256_permute2x128_si256(ascii1,ascii2,0x21);
    const __m256i first=skip==2U ? _mm256_alignr_epi8(cross01,ascii0,2)
                                 : _mm256_alignr_epi8(cross01,ascii0,3);
    const __m256i second=skip==2U ? _mm256_alignr_epi8(cross12,ascii1,2)
                                  : _mm256_alignr_epi8(cross12,ascii1,3);
    const __m128i tail_lo=_mm256_extracti128_si256(
        _mm256_alignr_epi8(cross12,ascii1,10),1);
    const __m128i tail_hi=_mm_alignr_epi8(
        _mm256_extracti128_si256(ascii2,1),_mm256_castsi256_si128(ascii2),10);
    const __m256i tail=_mm256_inserti128_si256(
        _mm256_castsi128_si256(tail_lo),tail_hi,1);
    const size_t length=90U-skip;
    _mm256_storeu_si256((__m256i *)(void *)output,first);
    _mm256_storeu_si256((__m256i *)(void *)(output+32),second);
    _mm256_storeu_si256((__m256i *)(void *)(output+length-32U),tail);
    output[length]='\0';
    return length;
  } else {
    uint64_t nz=(uint32_t)~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(d0,zero));
    nz|=(uint64_t)(uint32_t)~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(d1,zero))<<32;
    const uint32_t nz2=~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(d2,zero));
    const unsigned first=nz ? (unsigned)__builtin_ctzll(nz)
                            : (nz2 ? 64U+(unsigned)__builtin_ctz(nz2) : 90U);
    const __m256i in0=_mm256_loadu_si256((const __m256i *)(const void *)input);
    const __m256i in1=_mm256_loadu_si256((const __m256i *)(const void *)(input+32));
    const uint32_t bn0=~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(in0,zero));
    const uint32_t bn1=~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(in1,zero));
    const unsigned leading=bn0 ? (unsigned)__builtin_ctz(bn0)
        : (bn1 ? 32U+(unsigned)__builtin_ctz(bn1) : 64U);
    skip=first-leading;
  }
  _Alignas(32) uint8_t ascii[96];
  _mm256_store_si256((__m256i *)(void *)ascii,ascii0);
  _mm256_store_si256((__m256i *)(void *)(ascii+32),ascii1);
  _mm256_store_si256((__m256i *)(void *)(ascii+64),ascii2);
  const size_t length=90U-skip;
  _mm256_storeu_si256((__m256i *)(void *)output,
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii+skip)));
  _mm256_storeu_si256((__m256i *)(void *)(output+32),
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii+skip+32)));
  _mm256_storeu_si256((__m256i *)(void *)(output+length-32U),
      _mm256_loadu_si256((const __m256i *)(const void *)(ascii+58)));
  output[length]='\0';
  return length;
}

size_t braid58_encode_64_avx2(const uint8_t input[64], char output[89]) {
  return emit64(input,finish_raw64(make_raw64(input)),output);
}

void braid58_encode_64x2_avx2(const uint8_t input[2][64],
                              char output[2][89],size_t length[2]) {
  const raw64 r0=make_raw64(input[0]);
  const raw64 r1=make_raw64(input[1]);
  const cells64 c0=finish_raw64(r0);
  const cells64 c1=finish_raw64(r1);
  length[0]=emit64(input[0],c0,output[0]);
  length[1]=emit64(input[1],c1,output[1]);
}

void braid58_encode_64x3_avx2(const uint8_t input[3][64],
                              char output[3][89],size_t length[3]) {
  const raw64 r0=make_raw64(input[0]);
  const raw64 r1=make_raw64(input[1]);
  const raw64 r2=make_raw64(input[2]);
  const cells64 c0=finish_raw64(r0);
  const cells64 c1=finish_raw64(r1);
  const cells64 c2=finish_raw64(r2);
  length[0]=emit64(input[0],c0,output[0]);
  length[1]=emit64(input[1],c1,output[1]);
  length[2]=emit64(input[2],c2,output[2]);
}
