#ifndef BRAID58_H
#define BRAID58_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BRAID58_ENCODED_32_CAPACITY = 45 };

/* Encodes exactly 32 big-endian bytes using the Bitcoin Base58 alphabet.
   `output` receives a NUL-terminated string; the returned length excludes
   the terminator. */
size_t braid58_encode_32(const uint8_t input[32],
                         char output[BRAID58_ENCODED_32_CAPACITY]);

/* Decodes a canonical 32..44-character Bitcoin Base58 string that represents
   exactly 32 bytes. Returns 1 on success and 0 on invalid/noncanonical input;
   output is untouched on failure. */
int braid58_decode_32(const char *input, size_t length,
                      uint8_t output[32]);

/* Firedancer-compatible fixed-32 encoding surface. `optional_length` may be
   NULL. `output` must hold BRAID58_ENCODED_32_CAPACITY bytes; the function
   NUL-terminates it and returns `output`. */
char *fd_base58_encode_32(const void *bytes, unsigned long *optional_length,
                          char *output);

#ifdef __cplusplus
}
#endif

#endif /* BRAID58_H */
