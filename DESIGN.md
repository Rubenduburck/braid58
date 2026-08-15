# Braid58 design notes

## 1. Goal and contract

Braid58 explores a specialized AVX-512 circuit for the Bitcoin Base58 encoding
of one 256-bit value.  The complete encoder preserves Bitcoin's leading-zero
rule: every leading zero input byte becomes a leading `1`, including the
all-zero input.  The decoder accepts only canonical encodings that represent
exactly 32 output bytes.

The specialization is deliberate. Variable-width input and selectable
alphabets remain out of scope. The packaged public API adds runtime dispatch
and a scalar fallback around the specialized kernels.

## 2. Why direct base 29 was not the reduction

Because `58 = 2 * 29`, a radix-58 digit can be split into a base-29 digit and
one side bit.  For a block of `k` digits, let

```text
A = 29^k                 R = 58^k = 2^k A
c_i = 2^k q_i + r_i      0 <= r_i < 2^k.
```

Then division by `2^k` is local in radix `R`:

```text
X mod 2^k = r_0
c'_i      = q_i + A r_(i+1),       with c'_i < R.
```

This is a useful carry-free transducer, but it does not compress the complete
conversion.  Forty-four base-29 digits contain about 213.75 bits and the side
plane contains exactly another 44 bits.  Base 29 relocates those bits rather
than eliminating their recovery cost.

## 3. Chosen radix

The useful packing point is

```text
B = 58^6 = 38,068,692,544 < 2^36.
```

Six output symbols form one reasonably dense 36-bit cell, while eight such
cells cover the maximum 44-symbol representation with four spare symbol slots.
Eight cells also fit exactly in the eight 64-bit lanes of a ZMM register.

### 3.1 Encoding matrix

Interpret the big-endian input as an unsigned integer and rechunk it as

```text
X = sum(i=0..9) a_i 2^(26i),        0 <= a_i < 2^26.
```

For every input position, precompute the radix-`B` expansion

```text
2^(26i) = sum(j=0..7) w_ij B^j.
```

The raw output columns are therefore

```text
c_j = sum(i=0..9) a_i w_ij.
```

Rows 0 and 1 affect only the low column and are folded into its scalar seed;
the remaining rows are accumulated with 64-bit vector products.  An exhaustive
coefficient-bound calculation gives the largest possible raw column as

```text
11,454,759,584,224,480,191 = 0.6209638 * 2^64,
```

so every raw lane fits in an unsigned 64-bit word with ample room for the
subsequent bounded normalization carry.

Each lane is divided by the constant `B` using a floating-point reciprocal
estimate with an embedded round-to-nearest/no-exception mode.  Integer product
and comparison masks correct the estimate to the exact quotient and remainder,
independently of the caller's floating-point environment.  After each raw
quotient is added to the next lane, a lane sum is below `B + 2^29` and emits at
most one carry.  `sum >= B` generates a carry and `sum == B - 1` propagates an
incoming carry.  An eight-bit, three-stage prefix scan resolves the complete
chain without serial lane division.

Finally, each normalized `B` cell is split as

```text
58^6 = (58^3)^2,
58^3 = 58 * 58^2,
58^2 = 58 * 58,
```

again using exact constant-reciprocal operations.  The resulting digit bytes
are permuted and mapped through the fixed Bitcoin alphabet with VBMI byte
permutation.  The complete wrapper removes numeric padding and applies the
leading-zero-byte rule.

### 3.2 Decoding matrix

The decoder validates 32–44 input characters, classifies them through the
Bitcoin inverse alphabet, and groups the digits into eight radix-`B` cells.
It then applies the inverse fixed-radix transform:

```text
eight radix-58^6 cells  ->  five radix-2^52 limbs.
```

The weight matrix is accumulated with AVX-512 IFMA.  The limbs are normalized,
checked against the 256-bit bound and serialized as 32 big-endian bytes.  The
decoder also verifies the relationship between leading `1` characters and
leading zero bytes, rejecting noncanonical width, overflow, invalid alphabet
bytes, and unsupported lengths.

## 4. Instruction-set and implementation scope

The encoder uses AVX2 and AVX-512 F/DQ/BW/VL/IFMA/VBMI. The decoder additionally
uses VBMI2. These kernels are private target-attributed functions. The public
API checks the complete feature set and otherwise calls the scalar backend, so
the installed library is safe to run on unsupported CPUs.

The CMake build enables the optimized backend on x86-64 with GCC-compatible or
Clang-compatible compilers. Other configurations build scalar-only. The C ABI
documents non-overlapping buffers and failure behavior; the Rust wrapper makes
the common operations safe and allocation-free. Neither backend has received a
security audit or application-level fuzzing.

## 5. Correctness work completed

The bundled self-tests cover:

- Encoder differentials for 1,000,000 MSB-nonzero and 1,000,000 unrestricted
  values, plus all-zero, integer one, maximum value, and every leading-zero
  prefix. Public, scalar, and AVX-512 results are compared when available.
- Decoder round trips for the encoder corpus and 200,000 arbitrary
  32–44-character alphabet strings, including accept/reject differentials
  between the scalar and AVX-512 implementations.
- All 198 byte values outside the 58-character Bitcoin alphabet.
- Overflow, length, decoded-width, canonicality, NULL handling, known vectors,
  and the guarantee that failed decoding leaves output unchanged.
- Native and forced-scalar C and Rust builds.

These tests are meaningful evidence for the tested code, not a substitute for
continuous fuzzing or an independent audit.

## 6. Current benchmark record

[BENCHMARKS.md](BENCHMARKS.md) records the reproducible same-host comparison
against pinned Base58 Turbo, five8, and Firedancer versions. It measures the
runtime-dispatched public API, validates shared inputs before timing, and
reports medians across repeated trials. The results remain specific to the
machine, compiler, corpus, and cache state; they do not establish a universal
ordering.

## 7. Relationship to other work

Base58 Turbo was treated as a benchmark to beat, not as an implementation
template.  Its encoder converts input bytes into 22 radix-`58^2` 16-bit limbs
with an AVX2 `vpmaddwd` schedule and repeated carry rounds.  Braid58 instead
converts ten radix-`2^26` chunks into eight radix-`58^6` 64-bit lanes, performs
parallel reciprocal normalization, and resolves the residual carry dependency
as a small prefix problem.  The schedules and internal representations are
different.

Matrix radix conversion, close-radix decompositions, fixed-divisor reciprocal
division, and carry-lookahead scans are all established ideas.  This particular
`2^26 <-> 58^6` AVX-512 specialization was independently derived, and a limited
prior-art search did not find the same schedule.  That is not a patentability,
novelty, or freedom-to-operate conclusion.

## 8. Reproducing the local build

```sh
make test
make rust-test
make bench
```

The first command builds the C library and differential suite through CMake.
The second tests the safe Rust API. The benchmark builds the public Braid58 C
API alongside exact pinned competitors and pins the run to one logical CPU.
