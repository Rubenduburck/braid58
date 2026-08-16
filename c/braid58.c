#include "braid58_internal.h"

#include <string.h>

static const char BRAID58_ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

enum {
  B58_SCALAR_CHUNKS_32 = 10,
  B58_SCALAR_CHUNKS_64 = 20,
  B58_SCALAR_CELLS_32 = 9,
  B58_SCALAR_CELLS_64 = 18
};

#define B58_SCALAR_B2 UINT32_C(3364)
#define B58_SCALAR_B3 UINT32_C(195112)
#define B58_SCALAR_B5 UINT64_C(656356768)
#define B58_SCALAR_B10 UINT64_C(430804206899405824)
#define B58_SCALAR_MASK26 UINT64_C(0x3ffffff)

#if defined(__GNUC__) || defined(__clang__)
#define B58_SCALAR_INLINE __attribute__((always_inline)) inline
#define B58_SCALAR_NOINLINE __attribute__((noinline))
#else
#define B58_SCALAR_INLINE inline
#define B58_SCALAR_NOINLINE
#endif

/* Row i is the little-endian radix-58^5 expansion of 2^(26*i).
 * The scalar encoder shares this radix schedule with the AVX2 kernels, but
 * keeps the coefficients narrow and uses an ordinary carry chain. */
static const uint32_t B58_SCALAR_W5[B58_SCALAR_CHUNKS_64][17] = {
  {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {67108864,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {443814048,6861511,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {437973984,506963877,701551,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {192414240,584944171,527870455,71729,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {85356032,237320048,230466711,641497958,7333,0,0,0,0,0,0,0,0,0,0,0,0},
  {59086336,425716400,469620603,369434287,563670112,749,0,0,0,0,0,0,0,0,0,0,0},
  {249488768,563819044,543108756,589143927,28959436,439056932,76,0,0,0,0,0,0,0,0,0,0},
  {147129216,329522580,449746271,218521078,590921969,502289454,550667440,7,0,0,0,0,0,0,0,0,0},
  {576074656,432829647,553198108,378465102,185459504,598754100,313589673,526064760,0,0,0,0,0,0,0,0,0},
  {184037504,172044172,216479247,655243859,487688611,347228068,495642092,618120192,53787223,0,0,0,0,0,0,0,0},
  {11240928,599793205,368209896,143682386,101050947,216709650,476932781,529427649,237736760,5499447,0,0,0,0,0,0,0},
  {635074496,173439274,448067151,363842761,469972204,648159646,470224919,553330405,147241268,130740298,562288,0,0,0,0,0,0},
  {604481920,314192324,31450010,106032878,54664182,329747909,554128193,640039347,435587593,205535280,571695987,57490,0,0,0,0,0},
  {356764352,328895570,216399954,459733446,334397277,193013911,250503419,441894667,38393073,171253552,345880098,81961821,5878,0,0,0,0},
  {500650976,245129989,205412112,298516231,409710268,156613671,479004433,375386322,194031727,230544232,507785892,502831086,3865168,601,0,0,0},
  {308625792,104784612,644355351,184514320,45134568,144614682,467589845,453637228,212906911,527697587,239296485,517024870,141201404,295059608,61,0,0},
  {113587872,276429623,155974033,608422969,645780644,416442536,633095654,514800901,608440467,323200398,337103515,639109435,547702080,400446185,185668315,6,0},
  {141445696,172260198,484063438,78047805,442478329,289649177,191490987,298973999,163380196,293664278,348652084,170553025,507831179,427843872,341939960,421636746,0},
  {174092320,246601307,71321328,139080596,505046486,584509323,421142136,351377042,72896988,529815445,60242290,244603179,460450843,145566876,267323783,495067909,43110034}
};

static const uint8_t B58_SCALAR_W5_COLUMNS[B58_SCALAR_CHUNKS_64] = {
  1,1,2,3,4,5,6,7,8,8,9,10,11,12,13,14,15,16,16,17
};

/* Little-endian radix-58^5 expansions of 2^(32*i). The exact column
 * maxima stay below 2^64 through eleven source words. */
static const uint32_t B58_SCALAR_W32[11][11] = {
  {1,0,0,0,0,0,0,0,0,0,0},
  {356826688,6,0,0,0,0,0,0,0,0,0},
  {410450016,537767569,42,0,0,0,0,0,0,0,0},
  {357132832,389432875,127692781,280,0,0,0,0,0,0,0},
  {21339008,551597588,385795061,324463681,1833,0,0,0,0,0,0},
  {289024608,247894721,294005210,3737691,486083817,11997,0,0,0,0,0},
  {153715680,413102373,209184527,91512303,118408823,646269101,78508,0,0,0,0},
  {379377856,141436834,214625350,605448490,300156666,437087610,77223048,513735,0,0,0},
  {503769920,626087230,136596846,164019635,194569730,513969330,30977630,325788598,3361701,0,0},
  {44963712,430102516,160126051,574729546,404203788,210481832,595017589,148640294,294590275,21997789,0},
  {458949280,424550935,499113091,597442702,199595821,526964023,264290972,535878743,281429047,651677945,143945778}
};

static inline uint64_t b58_scalar_load_be64(const uint8_t *input) {
  return ((uint64_t)input[0] << 56) | ((uint64_t)input[1] << 48) |
         ((uint64_t)input[2] << 40) | ((uint64_t)input[3] << 32) |
         ((uint64_t)input[4] << 24) | ((uint64_t)input[5] << 16) |
         ((uint64_t)input[6] << 8) | input[7];
}

static inline uint32_t b58_scalar_load_be32(const uint8_t *input) {
  return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
         ((uint32_t)input[2] << 8) | input[3];
}

static inline void b58_scalar_store_be64(uint8_t *output, uint64_t word) {
  output[0] = (uint8_t)(word >> 56);
  output[1] = (uint8_t)(word >> 48);
  output[2] = (uint8_t)(word >> 40);
  output[3] = (uint8_t)(word >> 32);
  output[4] = (uint8_t)(word >> 24);
  output[5] = (uint8_t)(word >> 16);
  output[6] = (uint8_t)(word >> 8);
  output[7] = (uint8_t)word;
}

static inline size_t b58_scalar_word_bytes(uint64_t word) {
#if defined(__GNUC__) || defined(__clang__)
  return (size_t)(64U - (unsigned)__builtin_clzll(word) + 7U) / 8U;
#else
  size_t count = 0;
  do {
    ++count;
    word >>= 8;
  } while (word != 0);
  return count;
#endif
}

/* Two Base58 digits per lookup; 6.6 KiB and hot for fixed-width workloads. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverlength-strings"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
static const char B58_SCALAR_PAIR[] =
  "1112131415161718191A1B1C1D1E1F1G1H1J1K1L1M1N1P1Q1R1S1T1U1V1W1X1Y1Z1a1b1c1d1e1f1g1h1i1j1k1m1n1o1p1q1r1s1t1u1v1w1x1y1z"
  "2122232425262728292A2B2C2D2E2F2G2H2J2K2L2M2N2P2Q2R2S2T2U2V2W2X2Y2Z2a2b2c2d2e2f2g2h2i2j2k2m2n2o2p2q2r2s2t2u2v2w2x2y2z"
  "3132333435363738393A3B3C3D3E3F3G3H3J3K3L3M3N3P3Q3R3S3T3U3V3W3X3Y3Z3a3b3c3d3e3f3g3h3i3j3k3m3n3o3p3q3r3s3t3u3v3w3x3y3z"
  "4142434445464748494A4B4C4D4E4F4G4H4J4K4L4M4N4P4Q4R4S4T4U4V4W4X4Y4Z4a4b4c4d4e4f4g4h4i4j4k4m4n4o4p4q4r4s4t4u4v4w4x4y4z"
  "5152535455565758595A5B5C5D5E5F5G5H5J5K5L5M5N5P5Q5R5S5T5U5V5W5X5Y5Z5a5b5c5d5e5f5g5h5i5j5k5m5n5o5p5q5r5s5t5u5v5w5x5y5z"
  "6162636465666768696A6B6C6D6E6F6G6H6J6K6L6M6N6P6Q6R6S6T6U6V6W6X6Y6Z6a6b6c6d6e6f6g6h6i6j6k6m6n6o6p6q6r6s6t6u6v6w6x6y6z"
  "7172737475767778797A7B7C7D7E7F7G7H7J7K7L7M7N7P7Q7R7S7T7U7V7W7X7Y7Z7a7b7c7d7e7f7g7h7i7j7k7m7n7o7p7q7r7s7t7u7v7w7x7y7z"
  "8182838485868788898A8B8C8D8E8F8G8H8J8K8L8M8N8P8Q8R8S8T8U8V8W8X8Y8Z8a8b8c8d8e8f8g8h8i8j8k8m8n8o8p8q8r8s8t8u8v8w8x8y8z"
  "9192939495969798999A9B9C9D9E9F9G9H9J9K9L9M9N9P9Q9R9S9T9U9V9W9X9Y9Z9a9b9c9d9e9f9g9h9i9j9k9m9n9o9p9q9r9s9t9u9v9w9x9y9z"
  "A1A2A3A4A5A6A7A8A9AAABACADAEAFAGAHAJAKALAMANAPAQARASATAUAVAWAXAYAZAaAbAcAdAeAfAgAhAiAjAkAmAnAoApAqArAsAtAuAvAwAxAyAz"
  "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"
  "h1h2h3h4h5h6h7h8h9hAhBhChDhEhFhGhHhJhKhLhMhNhPhQhRhShThUhVhWhXhYhZhahbhchdhehfhghhhihjhkhmhnhohphqhrhshthuhvhwhxhyhz"
  "i1i2i3i4i5i6i7i8i9iAiBiCiDiEiFiGiHiJiKiLiMiNiPiQiRiSiTiUiViWiXiYiZiaibicidieifigihiiijikiminioipiqirisitiuiviwixiyiz"
  "j1j2j3j4j5j6j7j8j9jAjBjCjDjEjFjGjHjJjKjLjMjNjPjQjRjSjTjUjVjWjXjYjZjajbjcjdjejfjgjhjijjjkjmjnjojpjqjrjsjtjujvjwjxjyjz"
  "k1k2k3k4k5k6k7k8k9kAkBkCkDkEkFkGkHkJkKkLkMkNkPkQkRkSkTkUkVkWkXkYkZkakbkckdkekfkgkhkikjkkkmknkokpkqkrksktkukvkwkxkykz"
  "m1m2m3m4m5m6m7m8m9mAmBmCmDmEmFmGmHmJmKmLmMmNmPmQmRmSmTmUmVmWmXmYmZmambmcmdmemfmgmhmimjmkmmmnmompmqmrmsmtmumvmwmxmymz"
  "n1n2n3n4n5n6n7n8n9nAnBnCnDnEnFnGnHnJnKnLnMnNnPnQnRnSnTnUnVnWnXnYnZnanbncndnenfngnhninjnknmnnnonpnqnrnsntnunvnwnxnynz"
  "o1o2o3o4o5o6o7o8o9oAoBoCoDoEoFoGoHoJoKoLoMoNoPoQoRoSoToUoVoWoXoYoZoaobocodoeofogohoiojokomonooopoqorosotouovowoxoyoz"
  "p1p2p3p4p5p6p7p8p9pApBpCpDpEpFpGpHpJpKpLpMpNpPpQpRpSpTpUpVpWpXpYpZpapbpcpdpepfpgphpipjpkpmpnpopppqprpsptpupvpwpxpypz"
  "q1q2q3q4q5q6q7q8q9qAqBqCqDqEqFqGqHqJqKqLqMqNqPqQqRqSqTqUqVqWqXqYqZqaqbqcqdqeqfqgqhqiqjqkqmqnqoqpqqqrqsqtquqvqwqxqyqz"
  "r1r2r3r4r5r6r7r8r9rArBrCrDrErFrGrHrJrKrLrMrNrPrQrRrSrTrUrVrWrXrYrZrarbrcrdrerfrgrhrirjrkrmrnrorprqrrrsrtrurvrwrxryrz"
  "s1s2s3s4s5s6s7s8s9sAsBsCsDsEsFsGsHsJsKsLsMsNsPsQsRsSsTsUsVsWsXsYsZsasbscsdsesfsgshsisjsksmsnsospsqsrssstsusvswsxsysz"
  "t1t2t3t4t5t6t7t8t9tAtBtCtDtEtFtGtHtJtKtLtMtNtPtQtRtStTtUtVtWtXtYtZtatbtctdtetftgthtitjtktmtntotptqtrtstttutvtwtxtytz"
  "u1u2u3u4u5u6u7u8u9uAuBuCuDuEuFuGuHuJuKuLuMuNuPuQuRuSuTuUuVuWuXuYuZuaubucudueufuguhuiujukumunuoupuqurusutuuuvuwuxuyuz"
  "v1v2v3v4v5v6v7v8v9vAvBvCvDvEvFvGvHvJvKvLvMvNvPvQvRvSvTvUvVvWvXvYvZvavbvcvdvevfvgvhvivjvkvmvnvovpvqvrvsvtvuvvvwvxvyvz"
  "w1w2w3w4w5w6w7w8w9wAwBwCwDwEwFwGwHwJwKwLwMwNwPwQwRwSwTwUwVwWwXwYwZwawbwcwdwewfwgwhwiwjwkwmwnwowpwqwrwswtwuwvwwwxwywz"
  "x1x2x3x4x5x6x7x8x9xAxBxCxDxExFxGxHxJxKxLxMxNxPxQxRxSxTxUxVxWxXxYxZxaxbxcxdxexfxgxhxixjxkxmxnxoxpxqxrxsxtxuxvxwxxxyxz"
  "y1y2y3y4y5y6y7y8y9yAyByCyDyEyFyGyHyJyKyLyMyNyPyQyRySyTyUyVyWyXyYyZyaybycydyeyfygyhyiyjykymynyoypyqyrysytyuyvywyxyyyz"
  "z1z2z3z4z5z6z7z8z9zAzBzCzDzEzFzGzHzJzKzLzMzNzPzQzRzSzTzUzVzWzXzYzZzazbzczdzezfzgzhzizjzkzmznzozpzqzrzsztzuzvzwzxzyzz";
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static inline void b58_scalar_split5(uint32_t value, char output[5]) {
  const uint32_t first = value / UINT32_C(11316496);
  const uint32_t low4 = value - first * UINT32_C(11316496);
  const uint32_t high2 = low4 / B58_SCALAR_B2;
  const uint32_t low2 = low4 - high2 * B58_SCALAR_B2;
  output[0] = BRAID58_ALPHABET[first];
  memcpy(output + 1, B58_SCALAR_PAIR + 2U * high2, 2);
  memcpy(output + 3, B58_SCALAR_PAIR + 2U * low2, 2);
}

static inline unsigned b58_scalar_cell_digits(uint32_t value) {
  if (value >= UINT32_C(11316496))
    return 5;
  if (value >= B58_SCALAR_B3)
    return 4;
  if (value >= B58_SCALAR_B2)
    return 3;
  return value >= 58U ? 2U : 1U;
}

static size_t b58_scalar_emit(const uint32_t *cell, size_t cell_count,
                              size_t zero_count, char *output) {
  size_t top = cell_count;
  while (top > 1 && cell[top - 1U] == 0)
    --top;

  char first[5];
  b58_scalar_split5(cell[top - 1U], first);
  const unsigned first_digits = b58_scalar_cell_digits(cell[top - 1U]);
  const unsigned first_skip = 5U - first_digits;
  memset(output, '1', zero_count);
  memcpy(output + zero_count, first + first_skip, first_digits);
  size_t length = zero_count + first_digits;
  while (--top > 0) {
    b58_scalar_split5(cell[top - 1U], output + length);
    length += 5;
  }
  output[length] = '\0';
  return length;
}

static B58_SCALAR_INLINE size_t
b58_scalar_encode_w32(const uint8_t *input, size_t input_size,
                      size_t word_count, size_t zero_count, char *output) {
  uint32_t word[11];
  uint64_t raw[11] = {0};
  uint32_t cell[13];
  for (size_t i = 0; i < word_count; ++i)
    word[i] = b58_scalar_load_be32(
        input + input_size - 4U * (i + 1U));
  for (size_t row = 0; row < word_count; ++row) {
    const uint64_t value = word[row];
    for (size_t column = 0; column < word_count; ++column)
      raw[column] += value * B58_SCALAR_W32[row][column];
  }

  size_t cell_count = 0;
  uint64_t carry = 0;
  for (size_t column = 0; column < word_count; ++column) {
    const uint64_t value = raw[column] + carry;
    cell[cell_count++] = (uint32_t)(value % B58_SCALAR_B5);
    carry = value / B58_SCALAR_B5;
  }
  while (carry != 0) {
    cell[cell_count++] = (uint32_t)(carry % B58_SCALAR_B5);
    carry /= B58_SCALAR_B5;
  }
  return b58_scalar_emit(cell, cell_count, zero_count, output);
}

static B58_SCALAR_INLINE size_t
b58_scalar_encode_matrix(const uint8_t *input, size_t input_size,
                         size_t chunk_count, size_t zero_count, char *output) {
  const size_t word_count = input_size / 8U;
  const size_t raw_count = B58_SCALAR_W5_COLUMNS[chunk_count - 1U];
  const size_t cell_count = raw_count + 1U;
  uint64_t word[8];
  uint32_t chunk[B58_SCALAR_CHUNKS_64];
  uint64_t raw[B58_SCALAR_CELLS_64 - 1U] = {0};
  uint32_t cell[B58_SCALAR_CELLS_64];

  for (size_t i = 0; i < word_count; ++i)
    word[i] = b58_scalar_load_be64(input + input_size - 8U * (i + 1U));
  for (size_t i = 0; i < chunk_count; ++i) {
    const size_t bit = 26U * i;
    const size_t wi = bit >> 6;
    const unsigned offset = (unsigned)(bit & 63U);
    uint64_t value = word[wi] >> offset;
    if (offset > 38U && wi + 1U < word_count)
      value |= word[wi + 1U] << (64U - offset);
    chunk[i] = (uint32_t)(value & B58_SCALAR_MASK26);
  }

  raw[0] = (uint64_t)chunk[0] + ((uint64_t)chunk[1] << 26);
  if (chunk_count == B58_SCALAR_CHUNKS_32) {
    /* The rectangular loop gives portable compilers a profitable, regular
     * vectorization shape at this width, even though its upper triangle is
     * zero. */
    for (size_t row = 2; row < B58_SCALAR_CHUNKS_32; ++row)
      for (size_t column = 0; column + 1U < B58_SCALAR_CELLS_32; ++column)
        raw[column] += (uint64_t)chunk[row] * B58_SCALAR_W5[row][column];
  } else if (chunk_count == B58_SCALAR_CHUNKS_64) {
#define B58_SCALAR_ADD_ROW(ROW, COUNT)                                      \
  do {                                                                      \
    const uint64_t value = chunk[(ROW)];                                    \
    for (size_t column = 0; column < (COUNT); ++column)                     \
      raw[column] += value * B58_SCALAR_W5[(ROW)][column];                  \
  } while (0)
    B58_SCALAR_ADD_ROW(2, 2);
    B58_SCALAR_ADD_ROW(3, 3);
    B58_SCALAR_ADD_ROW(4, 4);
    B58_SCALAR_ADD_ROW(5, 5);
    B58_SCALAR_ADD_ROW(6, 6);
    B58_SCALAR_ADD_ROW(7, 7);
    B58_SCALAR_ADD_ROW(8, 8);
    B58_SCALAR_ADD_ROW(9, 8);
    B58_SCALAR_ADD_ROW(10, 9);
    B58_SCALAR_ADD_ROW(11, 10);
    B58_SCALAR_ADD_ROW(12, 11);
    B58_SCALAR_ADD_ROW(13, 12);
    B58_SCALAR_ADD_ROW(14, 13);
    B58_SCALAR_ADD_ROW(15, 14);
    B58_SCALAR_ADD_ROW(16, 15);
    B58_SCALAR_ADD_ROW(17, 16);
    B58_SCALAR_ADD_ROW(18, 16);
    B58_SCALAR_ADD_ROW(19, 17);
  } else {
    for (size_t row = 2; row < chunk_count; ++row)
      for (size_t column = 0;
           column < B58_SCALAR_W5_COLUMNS[row]; ++column)
        raw[column] += (uint64_t)chunk[row] * B58_SCALAR_W5[row][column];
  }
#undef B58_SCALAR_ADD_ROW

  uint64_t carry = 0;
  for (size_t i = 0; i < raw_count; ++i) {
    const uint64_t value = raw[i] + carry;
    cell[i] = (uint32_t)(value % B58_SCALAR_B5);
    carry = value / B58_SCALAR_B5;
  }
  cell[cell_count - 1U] = (uint32_t)carry;
  return b58_scalar_emit(cell, cell_count, zero_count, output);
}

static B58_SCALAR_NOINLINE size_t
b58_scalar_encode32_leading(const uint8_t input[32], char output[45]) {
  size_t zero_count = 1;
  while (zero_count < BRAID58_BINARY_32_SIZE && input[zero_count] == 0)
    ++zero_count;
  if (zero_count == BRAID58_BINARY_32_SIZE) {
    memset(output, '1', BRAID58_BINARY_32_SIZE);
    output[BRAID58_BINARY_32_SIZE] = '\0';
    return BRAID58_BINARY_32_SIZE;
  }
  const size_t word_count =
      (BRAID58_BINARY_32_SIZE - zero_count + 3U) / 4U;
#define B58_ENCODE32_CASE(WORDS)                                            \
  case (WORDS):                                                             \
    return b58_scalar_encode_w32(input, BRAID58_BINARY_32_SIZE,             \
                                 (WORDS), zero_count, output)
  switch (word_count) {
    B58_ENCODE32_CASE(1);
    B58_ENCODE32_CASE(2);
    B58_ENCODE32_CASE(3);
    B58_ENCODE32_CASE(4);
    B58_ENCODE32_CASE(5);
    B58_ENCODE32_CASE(6);
    B58_ENCODE32_CASE(7);
    B58_ENCODE32_CASE(8);
  default:
    return 0;
  }
#undef B58_ENCODE32_CASE
}

size_t
braid58_encode_32_scalar(const uint8_t input[static BRAID58_BINARY_32_SIZE],
                         char output[static BRAID58_ENCODED_32_CAPACITY]) {
  if (input[0] == 0)
    return b58_scalar_encode32_leading(input, output);
  return b58_scalar_encode_w32(input, BRAID58_BINARY_32_SIZE, 8, 0, output);
}

static B58_SCALAR_NOINLINE size_t
b58_scalar_encode64_leading(const uint8_t input[64], char output[89]) {
  size_t zero_count = 1;
  while (zero_count < BRAID58_BINARY_64_SIZE && input[zero_count] == 0)
    ++zero_count;
  if (zero_count == BRAID58_BINARY_64_SIZE) {
    memset(output, '1', BRAID58_BINARY_64_SIZE);
    output[BRAID58_BINARY_64_SIZE] = '\0';
    return BRAID58_BINARY_64_SIZE;
  }
  const size_t significant_bytes = BRAID58_BINARY_64_SIZE - zero_count;
  if (significant_bytes <= 44U) {
    const size_t word_count = (significant_bytes + 3U) / 4U;
#define B58_ENCODE64_W32_CASE(WORDS)                                        \
  case (WORDS):                                                             \
    return b58_scalar_encode_w32(input, BRAID58_BINARY_64_SIZE,             \
                                 (WORDS), zero_count, output)
    switch (word_count) {
      B58_ENCODE64_W32_CASE(1);
      B58_ENCODE64_W32_CASE(2);
      B58_ENCODE64_W32_CASE(3);
      B58_ENCODE64_W32_CASE(4);
      B58_ENCODE64_W32_CASE(5);
      B58_ENCODE64_W32_CASE(6);
      B58_ENCODE64_W32_CASE(7);
      B58_ENCODE64_W32_CASE(8);
      B58_ENCODE64_W32_CASE(9);
      B58_ENCODE64_W32_CASE(10);
      B58_ENCODE64_W32_CASE(11);
    default:
      return 0;
    }
#undef B58_ENCODE64_W32_CASE
  }
  size_t chunk_count =
      (significant_bytes * 8U + 25U) / 26U;
  chunk_count = (chunk_count + 1U) & ~(size_t)1U;
#define B58_ENCODE64_CASE(CHUNKS)                                           \
  case (CHUNKS):                                                            \
    return b58_scalar_encode_matrix(input, BRAID58_BINARY_64_SIZE,          \
                                    (CHUNKS), zero_count, output)
  switch (chunk_count) {
    B58_ENCODE64_CASE(14);
    B58_ENCODE64_CASE(16);
    B58_ENCODE64_CASE(18);
    B58_ENCODE64_CASE(20);
  default:
    return 0;
  }
#undef B58_ENCODE64_CASE
}

size_t
braid58_encode_64_scalar(const uint8_t input[static BRAID58_BINARY_64_SIZE],
                         char output[static BRAID58_ENCODED_64_CAPACITY]) {
  if (input[0] == 0)
    return b58_scalar_encode64_leading(input, output);
  return b58_scalar_encode_matrix(input, BRAID58_BINARY_64_SIZE,
                                  B58_SCALAR_CHUNKS_64, 0, output);
}

/* ASCII 0..127 to Bitcoin Base58; invalid entries have the high bit set. */
static const uint8_t B58_SCALAR_INV[128] = {
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,  0,  1,  2,  3,  4,  5,  6,  7,  8,255,255,255,255,255,255,
  255,  9, 10, 11, 12, 13, 14, 15, 16,255, 17, 18, 19, 20, 21,255,
   22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,255,255,255,255,255,
  255, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,255, 44, 45, 46,
   47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,255,255,255,255,255
};

static inline uint8_t b58_scalar_digit(uint8_t byte) {
  return (uint8_t)(B58_SCALAR_INV[byte & UINT8_C(0x7f)] |
                   (byte & UINT8_C(0x80)));
}

static B58_SCALAR_INLINE int b58_scalar_parse(const char *input, size_t length,
                                              uint64_t *value) {
  uint8_t bad = 0;
  uint64_t result = 0;
  if (length == 10) {
    const uint8_t d0 = b58_scalar_digit((uint8_t)input[0]);
    const uint8_t d1 = b58_scalar_digit((uint8_t)input[1]);
    const uint8_t d2 = b58_scalar_digit((uint8_t)input[2]);
    const uint8_t d3 = b58_scalar_digit((uint8_t)input[3]);
    const uint8_t d4 = b58_scalar_digit((uint8_t)input[4]);
    const uint8_t d5 = b58_scalar_digit((uint8_t)input[5]);
    const uint8_t d6 = b58_scalar_digit((uint8_t)input[6]);
    const uint8_t d7 = b58_scalar_digit((uint8_t)input[7]);
    const uint8_t d8 = b58_scalar_digit((uint8_t)input[8]);
    const uint8_t d9 = b58_scalar_digit((uint8_t)input[9]);
    bad = (uint8_t)(d0 | d1 | d2 | d3 | d4 | d5 | d6 | d7 | d8 | d9);
    const uint32_t v01 = (uint32_t)d0 * 58U + d1;
    const uint32_t v23 = (uint32_t)d2 * 58U + d3;
    const uint32_t v45 = (uint32_t)d4 * 58U + d5;
    const uint32_t v67 = (uint32_t)d6 * 58U + d7;
    const uint32_t v89 = (uint32_t)d8 * 58U + d9;
    const uint32_t v03 = v01 * B58_SCALAR_B2 + v23;
    const uint32_t v47 = v45 * B58_SCALAR_B2 + v67;
    result = ((uint64_t)v03 * UINT64_C(11316496) + v47) *
                 B58_SCALAR_B2 +
             v89;
  } else {
    for (size_t i = 0; i < length; ++i) {
      const uint8_t digit = b58_scalar_digit((uint8_t)input[i]);
      bad |= digit;
      result = result * 58U + digit;
    }
  }
  *value = result;
  return (bad & UINT8_C(0x80)) == 0;
}

#if defined(__SIZEOF_INT128__)
static int b58_scalar_mul_add(uint64_t *word, size_t *word_count,
                              size_t capacity, uint64_t multiplier,
                              uint64_t addend) {
  __uint128_t carry = addend;
  for (size_t i = 0; i < *word_count; ++i) {
    const __uint128_t product =
        (__uint128_t)word[i] * multiplier + carry;
    word[i] = (uint64_t)product;
    carry = product >> 64;
  }
  if (carry != 0) {
    if (*word_count == capacity)
      return 0;
    word[(*word_count)++] = (uint64_t)carry;
  }
  return 1;
}
#else
/* Portable 64-by-64 multiplication for compilers without a 128-bit integer. */
static int b58_scalar_mul_add(uint64_t *word64, size_t *word_count64,
                              size_t capacity64, uint64_t multiplier,
                              uint64_t addend) {
  const uint64_t multiplier_lo = (uint32_t)multiplier;
  const uint64_t multiplier_hi = multiplier >> 32;
  uint64_t carry = addend;
  for (size_t i = 0; i < *word_count64; ++i) {
    const uint64_t value = word64[i];
    const uint64_t value_lo = (uint32_t)value;
    const uint64_t value_hi = value >> 32;
    const uint64_t p00 = value_lo * multiplier_lo;
    const uint64_t p01 = value_lo * multiplier_hi;
    const uint64_t p10 = value_hi * multiplier_lo;
    uint64_t high = value_hi * multiplier_hi + (p01 >> 32) + (p10 >> 32);
    uint64_t low = p00;
    uint64_t next = low + (p01 << 32);
    high += next < low;
    low = next;
    next = low + (p10 << 32);
    high += next < low;
    low = next;
    next = low + carry;
    high += next < low;
    word64[i] = next;
    carry = high;
  }
  while (carry != 0) {
    if (*word_count64 == capacity64)
      return 0;
    word64[(*word_count64)++] = carry;
    carry = 0;
  }
  return 1;
}
#endif

static B58_SCALAR_INLINE int
b58_scalar_decode_full(const char *input, size_t length, uint8_t *output,
                       size_t output_size) {
  uint64_t word[BRAID58_BINARY_64_SIZE / 8U] = {0};
  uint8_t decoded[BRAID58_BINARY_64_SIZE];
  const size_t capacity = output_size / 8U;
  size_t word_count = 1;
  size_t first = length % 10U;
  if (first == 0)
    first = 10;
  if (!b58_scalar_parse(input, first, &word[0]))
    return 0;
  for (size_t offset = first; offset < length; offset += 10U) {
    uint64_t addend;
    if (!b58_scalar_parse(input + offset, 10, &addend) ||
        !b58_scalar_mul_add(word, &word_count, capacity,
                            B58_SCALAR_B10, addend))
      return 0;
  }
  for (size_t i = 0; i < capacity; ++i)
    b58_scalar_store_be64(decoded + output_size - 8U * (i + 1U), word[i]);
  if (decoded[0] == 0)
    return 0;
  memcpy(output, decoded, output_size);
  return 1;
}

static int braid58_decode_scalar(const char *input, size_t length,
                                 uint8_t *output, size_t output_size,
                                 size_t maximum_length) {
  uint64_t word[BRAID58_BINARY_64_SIZE / 8U] = {0};
  const size_t capacity = output_size / 8U;
  size_t leading_ones = 0;
  size_t word_count = 1;

  if (input == NULL || output == NULL || length < output_size ||
      length > maximum_length)
    return 0;
  while (leading_ones < length && input[leading_ones] == '1')
    ++leading_ones;

  const char *const payload = input + leading_ones;
  const size_t payload_length = length - leading_ones;
  if (payload_length == 0) {
    if (leading_ones != output_size)
      return 0;
    memset(output, 0, output_size);
    return 1;
  }

  size_t first = payload_length % 10U;
  if (first == 0)
    first = 10;
  if (!b58_scalar_parse(payload, first, &word[0]))
    return 0;
  for (size_t offset = first; offset < payload_length; offset += 10U) {
    uint64_t addend;
    if (!b58_scalar_parse(payload + offset, 10, &addend) ||
        !b58_scalar_mul_add(word, &word_count, capacity,
                            B58_SCALAR_B10, addend))
      return 0;
  }

  const size_t numeric_bytes = 8U * (word_count - 1U) +
                               b58_scalar_word_bytes(word[word_count - 1U]);
  if (leading_ones + numeric_bytes != output_size)
    return 0;
  memset(output, 0, output_size - 8U * word_count);
  for (size_t i = 0; i < word_count; ++i)
    b58_scalar_store_be64(output + output_size - 8U * (i + 1U), word[i]);
  return 1;
}

int braid58_decode_32_scalar(const char *input, size_t length,
                             uint8_t output[static BRAID58_BINARY_32_SIZE]) {
  if (input == NULL || output == NULL || length < BRAID58_BINARY_32_SIZE ||
      length > BRAID58_ENCODED_32_MAX_LEN)
    return 0;
  if (input[0] != '1')
    return b58_scalar_decode_full(input, length, output,
                                  BRAID58_BINARY_32_SIZE);
  return braid58_decode_scalar(input, length, output, BRAID58_BINARY_32_SIZE,
                               BRAID58_ENCODED_32_MAX_LEN);
}

int braid58_decode_64_scalar(const char *input, size_t length,
                             uint8_t output[static BRAID58_BINARY_64_SIZE]) {
  if (input == NULL || output == NULL || length < BRAID58_BINARY_64_SIZE ||
      length > BRAID58_ENCODED_64_MAX_LEN)
    return 0;
  if (input[0] != '1')
    return b58_scalar_decode_full(input, length, output,
                                  BRAID58_BINARY_64_SIZE);
  return braid58_decode_scalar(input, length, output, BRAID58_BINARY_64_SIZE,
                               BRAID58_ENCODED_64_MAX_LEN);
}

size_t braid58_encode_32(const uint8_t input[BRAID58_BINARY_32_SIZE],
                         char output[BRAID58_ENCODED_32_CAPACITY]) {
  if (input == NULL || output == NULL)
    return 0;
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512
  return braid58_encode_32_avx512(input, output);
#elif BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  return braid58_encode_32_avx2(input, output);
#else
  return braid58_encode_32_scalar(input, output);
#endif
}

void braid58_encode_32x2(
    const uint8_t input[2][BRAID58_BINARY_32_SIZE],
    char output[2][BRAID58_ENCODED_32_CAPACITY], size_t output_len[2]) {
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512
  braid58_encode_32x2_avx512(input, output, output_len);
#elif BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  braid58_encode_32x2_avx2(input, output, output_len);
#else
  output_len[0] = braid58_encode_32_scalar(input[0], output[0]);
  output_len[1] = braid58_encode_32_scalar(input[1], output[1]);
#endif
}

void braid58_encode_32x3(
    const uint8_t input[3][BRAID58_BINARY_32_SIZE],
    char output[3][BRAID58_ENCODED_32_CAPACITY], size_t output_len[3]) {
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512
  braid58_encode_32x3_avx512(input, output, output_len);
#elif BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  braid58_encode_32x3_avx2(input, output, output_len);
#else
  output_len[0] = braid58_encode_32_scalar(input[0], output[0]);
  output_len[1] = braid58_encode_32_scalar(input[1], output[1]);
  output_len[2] = braid58_encode_32_scalar(input[2], output[2]);
#endif
}

int braid58_decode_32(const char *input, size_t length,
                      uint8_t output[BRAID58_BINARY_32_SIZE]) {
  if (input == NULL || output == NULL)
    return 0;
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512
  return braid58_decode_32_avx512(input, length, output);
#elif BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  return braid58_decode_32_avx2(input, length, output);
#else
  return braid58_decode_32_scalar(input, length, output);
#endif
}

size_t braid58_encode_64(const uint8_t input[BRAID58_BINARY_64_SIZE],
                         char output[BRAID58_ENCODED_64_CAPACITY]) {
  if (input == NULL || output == NULL)
    return 0;
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512
  return braid58_encode_64_avx512(input, output);
#elif BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  return braid58_encode_64_avx2(input, output);
#else
  return braid58_encode_64_scalar(input, output);
#endif
}

void braid58_encode_64x2(
    const uint8_t input[2][BRAID58_BINARY_64_SIZE],
    char output[2][BRAID58_ENCODED_64_CAPACITY], size_t output_len[2]) {
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  braid58_encode_64x2_avx2(input, output, output_len);
#else
  output_len[0] = braid58_encode_64(input[0], output[0]);
  output_len[1] = braid58_encode_64(input[1], output[1]);
#endif
}

void braid58_encode_64x3(
    const uint8_t input[3][BRAID58_BINARY_64_SIZE],
    char output[3][BRAID58_ENCODED_64_CAPACITY], size_t output_len[3]) {
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  braid58_encode_64x3_avx2(input, output, output_len);
#else
  output_len[0] = braid58_encode_64(input[0], output[0]);
  output_len[1] = braid58_encode_64(input[1], output[1]);
  output_len[2] = braid58_encode_64(input[2], output[2]);
#endif
}

int braid58_decode_64(const char *input, size_t length,
                      uint8_t output[BRAID58_BINARY_64_SIZE]) {
  if (input == NULL || output == NULL)
    return 0;
#if BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512
  return braid58_decode_64_avx512(input, length, output);
#elif BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2
  return braid58_decode_64_avx2(input, length, output);
#else
  return braid58_decode_64_scalar(input, length, output);
#endif
}
