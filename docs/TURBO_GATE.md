# Base58 Turbo release gate

## Comparator

- Repository: `https://github.com/hacer-bark/base58-turbo.git`
- Version: 0.3.0
- Commit: `18c8f94eadfa5643dfd7e31b02250d3bf184fa68`
- Allowed patch: `bench/turbo-public-bridge.patch`
- Rust target: `znver5`
- Features: default (`unsafe-simd,std`)

The patch exports four C ABI wrappers around `BITCOIN.encode_into` and
`BITCOIN.decode_into`. It does not modify Turbo conversion code.

## Build

```sh
git clone https://github.com/hacer-bark/base58-turbo.git turbo-pinned
git -C turbo-pinned checkout 18c8f94eadfa5643dfd7e31b02250d3bf184fa68
git -C turbo-pinned apply /absolute/path/to/bench/turbo-public-bridge.patch
cd turbo-pinned
RUSTFLAGS='-C target-cpu=znver5' \
  cargo rustc --release --locked --lib -- --crate-type staticlib
```

The static archive is written to `target/release/deps/`.

## Run

```sh
taskset -c <physical-core> make turbo-gate \
  TURBO_DIR=/absolute/path/to/turbo-pinned \
  TURBO_LIB=/absolute/path/to/turbo-pinned/target/release/deps/libbase58_turbo-<hash>.a
```

## Checks

- Turbo HEAD equals the pinned commit.
- Turbo `src/simd.rs`, `src/encode.rs`, and `src/decode.rs` are unmodified.
- Both implementations use the same corpus and executable.
- Every output is checked before timing.
- The process is pinned to one CPU.
- Each function is warmed before measurement.
- Contender order alternates by trial.
- Each row reports the median of 17 paired trials.
- Corpora cover full-width and leading-zero inputs.
- Operations cover encode/decode at 32 and 64 bytes.

The program exits with status 1 unless Braid58 has lower median ticks in all
eight rows.

Record compiler versions, flags, linker, link order, CPU affinity, SMT state,
governor, microcode, and archive hashes with published results.
