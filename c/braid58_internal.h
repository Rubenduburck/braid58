#ifndef BRAID58_INTERNAL_H
#define BRAID58_INTERNAL_H

#include "braid58.h"

#define BRAID58_COMPILED_SCALAR 0
#define BRAID58_COMPILED_AVX2 1
#define BRAID58_COMPILED_AVX512 2

#ifndef BRAID58_COMPILED_TARGET
#define BRAID58_COMPILED_TARGET BRAID58_COMPILED_SCALAR
#endif

#if BRAID58_COMPILED_TARGET < BRAID58_COMPILED_SCALAR ||                  \
    BRAID58_COMPILED_TARGET > BRAID58_COMPILED_AVX512
#error "BRAID58_COMPILED_TARGET must be scalar (0), AVX2 (1), or AVX-512 (2)"
#endif

#define BRAID58_HAVE_AVX2_KERNEL                                           \
  (BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX2)

#define BRAID58_HAVE_AVX512_KERNEL                                         \
  (BRAID58_COMPILED_TARGET == BRAID58_COMPILED_AVX512)

size_t braid58_encode_32_scalar(const uint8_t input[BRAID58_BINARY_32_SIZE],
                                char output[BRAID58_ENCODED_32_CAPACITY]);
int braid58_decode_32_scalar(const char *input, size_t length,
                             uint8_t output[BRAID58_BINARY_32_SIZE]);
size_t braid58_encode_64_scalar(const uint8_t input[BRAID58_BINARY_64_SIZE],
                                char output[BRAID58_ENCODED_64_CAPACITY]);
int braid58_decode_64_scalar(const char *input, size_t length,
                             uint8_t output[BRAID58_BINARY_64_SIZE]);

#if BRAID58_HAVE_AVX2_KERNEL
size_t braid58_encode_32_avx2(const uint8_t input[BRAID58_BINARY_32_SIZE],
                              char output[BRAID58_ENCODED_32_CAPACITY]);
void braid58_encode_32x2_avx2(
    const uint8_t input[2][BRAID58_BINARY_32_SIZE],
    char output[2][BRAID58_ENCODED_32_CAPACITY], size_t output_len[2]);
void braid58_encode_32x3_avx2(
    const uint8_t input[3][BRAID58_BINARY_32_SIZE],
    char output[3][BRAID58_ENCODED_32_CAPACITY], size_t output_len[3]);
int braid58_decode_32_avx2(const char *input, size_t length,
                           uint8_t output[BRAID58_BINARY_32_SIZE]);
size_t braid58_encode_64_avx2(const uint8_t input[BRAID58_BINARY_64_SIZE],
                              char output[BRAID58_ENCODED_64_CAPACITY]);
void braid58_encode_64x2_avx2(
    const uint8_t input[2][BRAID58_BINARY_64_SIZE],
    char output[2][BRAID58_ENCODED_64_CAPACITY], size_t output_len[2]);
void braid58_encode_64x3_avx2(
    const uint8_t input[3][BRAID58_BINARY_64_SIZE],
    char output[3][BRAID58_ENCODED_64_CAPACITY], size_t output_len[3]);
int braid58_decode_64_avx2(const char *input, size_t length,
                           uint8_t output[BRAID58_BINARY_64_SIZE]);
#endif

#if BRAID58_HAVE_AVX512_KERNEL
size_t braid58_encode_32_avx512(const uint8_t input[BRAID58_BINARY_32_SIZE],
                                char output[BRAID58_ENCODED_32_CAPACITY]);
void braid58_encode_32x2_avx512(
    const uint8_t input[2][BRAID58_BINARY_32_SIZE],
    char output[2][BRAID58_ENCODED_32_CAPACITY], size_t output_len[2]);
void braid58_encode_32x3_avx512(
    const uint8_t input[3][BRAID58_BINARY_32_SIZE],
    char output[3][BRAID58_ENCODED_32_CAPACITY], size_t output_len[3]);
int braid58_decode_32_avx512(const char *input, size_t length,
                             uint8_t output[BRAID58_BINARY_32_SIZE]);
size_t braid58_encode_64_avx512(const uint8_t input[BRAID58_BINARY_64_SIZE],
                                char output[BRAID58_ENCODED_64_CAPACITY]);
int braid58_decode_64_avx512(const char *input, size_t length,
                             uint8_t output[BRAID58_BINARY_64_SIZE]);
#endif

#endif /* BRAID58_INTERNAL_H */
