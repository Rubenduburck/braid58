# Braid58

**Parallel radix lifting for fixed-width Base58.**

Braid58 is an independent research prototype for encoding and decoding exactly
32 bytes with the Bitcoin Base58 alphabet.  Its internal radix is

```text
B = 58^6 = 2^6 * 29^6 = 38,068,692,544.
```

The encoder converts ten radix-`2^26` chunks into eight radix-`B` columns held
in one ZMM register.  The decoder reverses the construction, mapping eight
radix-`B` cells to five radix-`2^52` limbs.  See [DESIGN.md](DESIGN.md) for the
derivation, bounds, validation coverage, and benchmark caveats.

## Contents

- `src/encode_r6.c` — encoder, Bitcoin leading-zero semantics, compatibility
  wrapper, and embedded correctness tests.
- `src/decode_r6.c` — strict decoder for canonical 32–44-character encodings
  whose decoded width is exactly 32 bytes.
- `include/braid58.h` — public prototype declarations and output capacity.
- `Makefile` — static-library and self-test targets.
- `DESIGN.md` — algorithm and experimental record.
- `BENCHMARKS.md` — reproducible same-host Firedancer comparison and results.
- `bench/` — validation and invariant-TSC benchmark harness.

## Public prototype APIs

```c
#include "braid58.h"

/* Writes 32..44 characters followed by NUL; returns length excluding NUL. */
size_t braid58_encode_32(const uint8_t in[32], char out[45]);

/* Firedancer-compatible wrapper; opt_len may be NULL and the return is out. */
char *fd_base58_encode_32(const void *in, unsigned long *opt_len, char out[45]);

/* Returns 1 on success, 0 on failure; failure leaves out unchanged. */
int braid58_decode_32(const char *in, size_t len, uint8_t out[32]);
```

The C sources retain a few historical `radix6_*` compatibility symbols for the
development harnesses; they are intentionally absent from the public header.
The declared API is still a prototype rather than a stable ABI.

## Build and verify

The encoder requires AVX2 and AVX-512 F, DQ, BW, VL, IFMA and VBMI.  The
decoder additionally requires VBMI2.  Build only on a machine where
`-march=native` enables those features.

```sh
make test                  # build and run both embedded test programs
make                       # build libbraid58.a
make bench                 # compare with Turbo, five8, and Firedancer
```

These run the embedded differential, leading-zero, API-contract, and rejection
tests.  The Makefile compiles the library encoder with `-DBRAID58_NO_MAIN`.
Equivalent explicit compiler commands are in `DESIGN.md`.

The benchmark target fetches the pinned official Firedancer source and exact
Base58 Turbo and five8 crates on its first run, builds all implementations for
the native CPU, validates their results, pins execution to one logical CPU,
and reports repeated invariant-TSC ticks, calls per second, and GiB/s.  Set
`FIREDANCER_DIR` to use an existing checkout, or tune a run with `BENCH_CPU`,
`BENCH_ITERATIONS`, and `BENCH_TRIALS`.  Braid58 only implements the fixed-32
APIs; the fixed-64 results are standalone baselines rather than direct
comparisons.

## Scope

- Fixed 32-byte input/output only.
- Bitcoin alphabet only.
- No run-time CPU dispatch and no portable fallback.
- Research code: not hardened, audited, or promised constant-time.
- The reported benchmarks are hot-cache results from one AVX-512 host, not
  universal performance claims.

The design was derived independently.  Base58 Turbo was the historical
comparison target, not a source for this implementation.  The original
historical comparison harness was absent from the archive; `bench/` is the new
reproducible replacement.  Standard ingredients such as matrix radix
conversion, reciprocal division by constants, and carry lookahead are
established techniques; no claim of general algorithmic novelty is made.
