#include "braid58_internal.h"

#include <string.h>

static const char BRAID58_ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static size_t braid58_encode_scalar(const uint8_t *input, size_t input_size,
                                    char *output) {
  uint8_t work[BRAID58_BINARY_64_SIZE];
  uint8_t reverse[BRAID58_ENCODED_64_MAX_LEN];
  size_t zero_count = 0;
  size_t digit_count = 0;

  memcpy(work, input, input_size);
  while (zero_count < input_size && work[zero_count] == 0)
    ++zero_count;

  size_t start = zero_count;
  while (start < input_size) {
    unsigned remainder = 0;
    for (size_t i = start; i < input_size; ++i) {
      const unsigned value = remainder * 256U + work[i];
      work[i] = (uint8_t)(value / 58U);
      remainder = value % 58U;
    }
    reverse[digit_count++] = (uint8_t)remainder;
    while (start < input_size && work[start] == 0)
      ++start;
  }

  memset(output, '1', zero_count);
  for (size_t i = 0; i < digit_count; ++i)
    output[zero_count + i] = BRAID58_ALPHABET[reverse[digit_count - 1U - i]];

  const size_t length = zero_count + digit_count;
  output[length] = '\0';
  return length;
}

size_t
braid58_encode_32_scalar(const uint8_t input[static BRAID58_BINARY_32_SIZE],
                         char output[static BRAID58_ENCODED_32_CAPACITY]) {
  return braid58_encode_scalar(input, BRAID58_BINARY_32_SIZE, output);
}

size_t
braid58_encode_64_scalar(const uint8_t input[static BRAID58_BINARY_64_SIZE],
                         char output[static BRAID58_ENCODED_64_CAPACITY]) {
  return braid58_encode_scalar(input, BRAID58_BINARY_64_SIZE, output);
}

static int braid58_decode_scalar(const char *input, size_t length,
                                 uint8_t *output, size_t output_size,
                                 size_t maximum_length) {
  uint8_t decoded[BRAID58_BINARY_64_SIZE] = {0};
  size_t leading_ones = 0;

  if (input == NULL || output == NULL || length < output_size ||
      length > maximum_length)
    return 0;

  while (leading_ones < length && input[leading_ones] == '1')
    ++leading_ones;

  for (size_t i = 0; i < length; ++i) {
    const uint8_t byte = (uint8_t)input[i];
    const char *const found =
        byte == 0 ? NULL : (const char *)memchr(BRAID58_ALPHABET, byte, 58);
    if (found == NULL)
      return 0;

    unsigned carry = (unsigned)(found - BRAID58_ALPHABET);
    for (size_t j = output_size; j-- > 0;) {
      const unsigned value = decoded[j] * 58U + carry;
      decoded[j] = (uint8_t)value;
      carry = value >> 8;
    }
    if (carry != 0)
      return 0;
  }

  size_t leading_zero_bytes = 0;
  while (leading_zero_bytes < output_size && decoded[leading_zero_bytes] == 0)
    ++leading_zero_bytes;
  if (leading_ones != leading_zero_bytes)
    return 0;

  memcpy(output, decoded, output_size);
  return 1;
}

int braid58_decode_32_scalar(const char *input, size_t length,
                             uint8_t output[static BRAID58_BINARY_32_SIZE]) {
  return braid58_decode_scalar(input, length, output, BRAID58_BINARY_32_SIZE,
                               BRAID58_ENCODED_32_MAX_LEN);
}

int braid58_decode_64_scalar(const char *input, size_t length,
                             uint8_t output[static BRAID58_BINARY_64_SIZE]) {
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
