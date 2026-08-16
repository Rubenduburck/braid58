# Design

## Interface constraints

- Alphabet: Bitcoin Base58.
- Binary widths: 32 and 64 bytes.
- Byte order: big-endian.
- Encoded form: canonical Bitcoin Base58; C output includes a trailing NUL.
- Decode failure: output is not modified.
- C input/output overlap: unsupported.
- Timing: data-dependent.
- Backend selection: compile time.

## Selected kernels

| Target | Encode32 | Decode32 | Encode64 | Decode64 |
|---|---|---|---|---|
| AVX2 | B5 | B4 | B5 | B4 |
| Threadripper PRO 9995WX | ZMM B6 | mixed ZMM B4 | ZMM B5 | mixed ZMM/YMM B4 |

## Encode32

The AVX2 encoder rechunks the 256-bit input into ten radix-`2^26` values and
applies a precomputed matrix to produce nine radix-`58^5` columns. Since
`58^5 < 2^30`, `VPMULUDQ` computes each 26-by-30-bit product without product
splitting. A parallel carry scan normalizes the columns before digit emission.

The AVX-512 encoder uses the same ten input chunks and eight radix-`58^6`
columns. Eight columns occupy one ZMM register. Constant-reciprocal division,
integer quotient correction, and an eight-lane carry lookahead normalize the
columns. VBMI performs digit ordering and alphabet lookup.

## Decode32

Both SIMD targets group the input into eleven radix-`58^4` cells with
`VPMADDUBSW` and `VPMADDWD`, then apply a precomputed matrix to produce eight
radix-`2^32` words.

The AVX2 kernel uses YMM matrix rows. The AVX-512 kernel maps and folds the
44-byte frame in a ZMM and selects ZMM, YMM, or scalar products by row density.

## Encode64

The AVX2 encoder maps twenty radix-`2^26` chunks to radix-`58^5` columns.

The AVX-512 encoder produces seventeen raw radix-`58^5` columns and one top
carry. The maximum raw column is below `2^59`. The matrix stage uses 29 ZMM
`VPMULUDQ` products.

## Decode64

The AVX2 decoder folds 88 digits into twenty-two radix-`58^4` cells, applies a
sparse inverse matrix, and emits sixteen radix-`2^32` words.

The AVX-512 decoder uses two overlapping 64-byte loads, VBMI mapping,
`VPMADDUBSW`/`VPMADDWD` folding, density-specific ZMM/YMM/scalar matrix rows,
a sixteen-lane carry lookahead, and one final byte permutation/store.

## Rejected kernels

The following measured slower than the selected kernels:

- B6/IFMA decode;
- B6 encode64 using `VPMULLQ`;
- B6 encode64 using IFMA;
- all-ZMM sparse matrices;
- B8/BMI2 decode;
- P27 and P29 encode layouts;
- serial carry propagation;
- alternate carry-prefix schedules;
- alternate parser schedules;
- YMM alias bodies on the AVX-512 target.

The object audit rejects `VPMULLQ`, IFMA, and VBMI instructions in the
selected AVX-512 encode64 object and rejects IFMA and `VPDPBUSD` in the
selected AVX-512 decode64 object.

## Batch scope

Encode32 has dedicated staged x2/x3 kernels on AVX2 and AVX-512. Encode64 has
dedicated staged x2/x3 kernels on AVX2. The public scalar and AVX-512
Encode64 batch entry points preserve the same API by calling the selected
single encoder per lane; no dedicated AVX-512 Encode64 batch kernel is
provided.

Source hashes are listed in [docs/PROVENANCE.md](docs/PROVENANCE.md).
