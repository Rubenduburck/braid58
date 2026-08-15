#ifndef BRAID58_INTERNAL_H
#define BRAID58_INTERNAL_H

#include "braid58.h"

size_t braid58_encode_32_scalar(
    const uint8_t input[BRAID58_BINARY_32_SIZE],
    char output[BRAID58_ENCODED_32_CAPACITY]);
int braid58_decode_32_scalar(const char *input, size_t length,
                             uint8_t output[BRAID58_BINARY_32_SIZE]);

#if BRAID58_HAVE_AVX2_KERNEL
size_t braid58_encode_32_avx2(
    const uint8_t input[BRAID58_BINARY_32_SIZE],
    char output[BRAID58_ENCODED_32_CAPACITY]);
int braid58_decode_32_avx2(const char *input, size_t length,
                           uint8_t output[BRAID58_BINARY_32_SIZE]);
#endif

#if BRAID58_HAVE_AVX512_KERNEL
size_t braid58_encode_32_avx512(
    const uint8_t input[BRAID58_BINARY_32_SIZE],
    char output[BRAID58_ENCODED_32_CAPACITY]);
int braid58_decode_32_avx512(const char *input, size_t length,
                             uint8_t output[BRAID58_BINARY_32_SIZE]);
#endif

#endif /* BRAID58_INTERNAL_H */
