#include "braid58_internal.h"

#include <string.h>

static const char BRAID58_ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

braid58_backend
braid58_get_backend(void) {
#if BRAID58_HAVE_AVX512_KERNEL &&                                    \
    (defined(__GNUC__) || defined(__clang__)) &&                    \
    (defined(__x86_64__) || defined(__i386__))
  if (__builtin_cpu_supports("avx2") &&
      __builtin_cpu_supports("avx512f") &&
      __builtin_cpu_supports("avx512dq") &&
      __builtin_cpu_supports("avx512bw") &&
      __builtin_cpu_supports("avx512vl") &&
      __builtin_cpu_supports("avx512ifma") &&
      __builtin_cpu_supports("avx512vbmi") &&
      __builtin_cpu_supports("avx512vbmi2"))
    return BRAID58_BACKEND_AVX512;
#endif
#if BRAID58_HAVE_AVX2_KERNEL &&                                     \
    (defined(__GNUC__) || defined(__clang__)) &&                    \
    (defined(__x86_64__) || defined(__i386__))
  if (__builtin_cpu_supports("avx2"))
    return BRAID58_BACKEND_AVX2;
#endif
  return BRAID58_BACKEND_SCALAR;
}

size_t
braid58_encode_32_scalar(const uint8_t input[static BRAID58_BINARY_32_SIZE],
                         char output[static BRAID58_ENCODED_32_CAPACITY]) {
  uint8_t work[BRAID58_BINARY_32_SIZE];
  uint8_t reverse[BRAID58_ENCODED_32_MAX_LEN];
  size_t zero_count = 0;
  size_t digit_count = 0;

  memcpy(work, input, sizeof(work));
  while (zero_count < sizeof(work) && work[zero_count] == 0)
    ++zero_count;

  size_t start = zero_count;
  while (start < sizeof(work)) {
    unsigned remainder = 0;
    for (size_t i = start; i < sizeof(work); ++i) {
      const unsigned value = remainder * 256U + work[i];
      work[i] = (uint8_t)(value / 58U);
      remainder = value % 58U;
    }
    reverse[digit_count++] = (uint8_t)remainder;
    while (start < sizeof(work) && work[start] == 0)
      ++start;
  }

  memset(output, '1', zero_count);
  for (size_t i = 0; i < digit_count; ++i)
    output[zero_count + i] = BRAID58_ALPHABET[reverse[digit_count - 1U - i]];

  const size_t length = zero_count + digit_count;
  output[length] = '\0';
  return length;
}

int
braid58_decode_32_scalar(const char *input, size_t length,
                         uint8_t output[static BRAID58_BINARY_32_SIZE]) {
  uint8_t decoded[BRAID58_BINARY_32_SIZE] = {0};
  size_t leading_ones = 0;

  if (input == NULL || output == NULL ||
      length < BRAID58_BINARY_32_SIZE ||
      length > BRAID58_ENCODED_32_MAX_LEN)
    return 0;

  while (leading_ones < length && input[leading_ones] == '1')
    ++leading_ones;

  for (size_t i = 0; i < length; ++i) {
    const uint8_t byte = (uint8_t)input[i];
    const char *const found = byte == 0 ? NULL :
        (const char *)memchr(BRAID58_ALPHABET, byte, 58);
    if (found == NULL)
      return 0;

    unsigned carry = (unsigned)(found - BRAID58_ALPHABET);
    for (size_t j = sizeof(decoded); j-- > 0;) {
      const unsigned value = decoded[j] * 58U + carry;
      decoded[j] = (uint8_t)value;
      carry = value >> 8;
    }
    if (carry != 0)
      return 0;
  }

  size_t leading_zero_bytes = 0;
  while (leading_zero_bytes < sizeof(decoded) &&
         decoded[leading_zero_bytes] == 0)
    ++leading_zero_bytes;
  if (leading_ones != leading_zero_bytes)
    return 0;

  memcpy(output, decoded, sizeof(decoded));
  return 1;
}

size_t
braid58_encode_32(const uint8_t input[BRAID58_BINARY_32_SIZE],
                  char output[BRAID58_ENCODED_32_CAPACITY]) {
  if (input == NULL || output == NULL)
    return 0;
#if BRAID58_HAVE_AVX2_KERNEL || BRAID58_HAVE_AVX512_KERNEL
  const braid58_backend backend = braid58_get_backend();
#endif
#if BRAID58_HAVE_AVX512_KERNEL
  if (backend == BRAID58_BACKEND_AVX512)
    return braid58_encode_32_avx512(input, output);
#endif
#if BRAID58_HAVE_AVX2_KERNEL
  if (backend == BRAID58_BACKEND_AVX2)
    return braid58_encode_32_avx2(input, output);
#endif
  return braid58_encode_32_scalar(input, output);
}

int
braid58_decode_32(const char *input, size_t length,
                  uint8_t output[BRAID58_BINARY_32_SIZE]) {
  if (input == NULL || output == NULL)
    return 0;
#if BRAID58_HAVE_AVX2_KERNEL || BRAID58_HAVE_AVX512_KERNEL
  const braid58_backend backend = braid58_get_backend();
#endif
#if BRAID58_HAVE_AVX512_KERNEL
  if (backend == BRAID58_BACKEND_AVX512)
    return braid58_decode_32_avx512(input, length, output);
#endif
#if BRAID58_HAVE_AVX2_KERNEL
  if (backend == BRAID58_BACKEND_AVX2)
    return braid58_decode_32_avx2(input, length, output);
#endif
  return braid58_decode_32_scalar(input, length, output);
}
