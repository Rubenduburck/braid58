#ifndef BRAID58_H
#define BRAID58_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(BRAID58_SHARED)
#  if defined(BRAID58_BUILDING_LIBRARY)
#    define BRAID58_API __declspec(dllexport)
#  else
#    define BRAID58_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define BRAID58_API __attribute__((visibility("default")))
#else
#  define BRAID58_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BRAID58_VERSION_MAJOR 0
#define BRAID58_VERSION_MINOR 1
#define BRAID58_VERSION_PATCH 0
#define BRAID58_VERSION_STRING "0.1.0"

enum {
  BRAID58_BINARY_32_SIZE = 32,
  BRAID58_ENCODED_32_MAX_LEN = 44,
  BRAID58_ENCODED_32_CAPACITY = 45
};

typedef enum braid58_backend {
  BRAID58_BACKEND_SCALAR = 0,
  BRAID58_BACKEND_AVX2 = 1,
  BRAID58_BACKEND_AVX512 = 2
} braid58_backend;

/* Returns the implementation selected for this process and CPU. */
BRAID58_API braid58_backend braid58_get_backend(void);

/* Encodes exactly 32 bytes with the Bitcoin Base58 alphabet.

   output receives a NUL-terminated string. The return value is its length,
   excluding the terminator, or zero when input or output is NULL. Input and
   output must not overlap. */
BRAID58_API size_t
braid58_encode_32(const uint8_t input[BRAID58_BINARY_32_SIZE],
                  char output[BRAID58_ENCODED_32_CAPACITY]);

/* Decodes a canonical 32..44-byte Bitcoin Base58 string representing exactly
   32 bytes. Returns one on success and zero on invalid input. output is left
   unchanged on failure. Input and output must not overlap. */
BRAID58_API int
braid58_decode_32(const char *input, size_t length,
                  uint8_t output[BRAID58_BINARY_32_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* BRAID58_H */
