# Braid58 design notes

## 1. Goal and contract

Braid58 explores a specialized AVX-512 circuit for the Bitcoin Base58 encoding
of one 256-bit value.  The complete encoder preserves Bitcoin's leading-zero
rule: every leading zero input byte becomes a leading `1`, including the
all-zero input.  The decoder accepts only canonical encodings that represent
exactly 32 output bytes.

The specialization is deliberate.  Variable-width input, selectable alphabets,
portable dispatch, and non-AVX-512 fallbacks are outside this prototype.

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

The encoder uses AVX2 and AVX-512 F/DQ/BW/VL/IFMA/VBMI.  The decoder
additionally uses VBMI2.  There is no feature check inside the prototype:
executing it on a CPU without the required extensions is invalid.  There is
also no portable scalar fallback, alternate alphabet, variable-width path,
stable ABI, or security audit.

The kernels are intended to demonstrate the radix schedule.  A production
library still needs CPU dispatch, fallback implementations, supported-compiler
testing, a documented overlap policy, and application-level fuzzing.

## 5. Correctness work completed

The bundled self-tests cover:

- Encoder differential tests against straightforward repeated division:
  1,000,000 MSB-nonzero cases and 1,000,000 unrestricted cases with rotating
  forced zero prefixes, plus all-zero, integer one, maximum value, and one
  deliberate case for every leading-zero prefix.  The executable reports
  2,000,036 differential/API cases.
- Every possible leading-zero prefix, the all-zero vector, output boundaries,
  and the compatibility wrapper's terminator, optional length, and return
  pointer.
- Decoder differentials for 200,000 valid round trips (cycling through leading
  zero runs) and 200,000 arbitrary 32–44-character alphabet strings, including
  their accept/reject outcome.
- All 198 byte values outside the 58-character Bitcoin alphabet.
- Overflow, length, decoded-width, and canonicality rejection cases.

These tests are meaningful evidence for the tested code, not a substitute for
continuous fuzzing or an independent audit.

## 6. Current benchmark record

The current rebuilt kernels were measured in hot-cache microbenchmarks on an
AMD EPYC 9V74, with work pinned to a core and native-target optimized builds.
The unit is an invariant-TSC tick; it is not necessarily a core cycle or a
nanosecond.  Base58 Turbo 0.3.0's direct fixed-32 kernels are retained as the
same-host comparison ranges recorded during development.

| Fixed-32 operation | Current Braid58 | Turbo direct | Range-relative reduction |
|---|---:|---:|---:|
| Encode, complete public path | 71.7–73.0 | 80.58–81.41 | 9–12% |
| Decode, common 44-character input | 60.26–61.45 | 73.10–75.80 | 16–20% |

These numbers describe this machine, compiler configuration, benchmark corpus,
and cache state only.  They do not establish a universal ordering.  In
particular, CPUs with different AVX-512 implementations, frequency behavior,
or no AVX-512 support can produce a very different result.  End-to-end
application measurements should decide deployment.  The decoder row is the
current rebuilt kernel; the Turbo range is the same-host historical comparator
and should be remeasured alongside it before making a deployment decision.
Occasional decoder samples around 62–63 ticks were attributed to system noise;
the table gives the stable pinned-run range.  No current varied-length decoder
measurement is claimed.

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

Run the embedded encoder verification:

```sh
gcc -O3 -march=native -fno-stack-protector \
  -Wall -Wextra -Werror src/encode_r6.c -o encode_r6_test
./encode_r6_test

gcc -O3 -march=native -DBRAID58_TEST \
  -Wall -Wextra -Werror src/decode_r6.c -o decode_r6_test
./decode_r6_test
```

Compile objects for integration with a harness:

```sh
gcc -O3 -march=native -fno-stack-protector -DBRAID58_NO_MAIN \
  -Wall -Wextra -Werror -c src/encode_r6.c -o encode_r6.o
gcc -O3 -march=native \
  -Wall -Wextra -Werror -c src/decode_r6.c -o decode_r6.o
ar rcs libbraid58.a encode_r6.o decode_r6.o
```

The historical Base58 Turbo comparison harness is not included in this bundle.
To reproduce the table rather than merely test Braid58, construct an equivalent
fixed-input benchmark against Base58 Turbo 0.3.0, link `libbraid58.a`, use a
native release build, pin execution to a chosen core, and report repeated run
ranges rather than a single best sample.  Treat the recorded Turbo range as
context until both implementations are rebuilt and measured in the same run.
