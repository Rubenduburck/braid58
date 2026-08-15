# Local benchmark record

## 2026-08-15 comparison with Base58 Turbo, five8, and Firedancer

The repository benchmark was run on an AMD Ryzen Threadripper PRO 9995WX
96-Core, pinned to logical CPU 31.  Braid58, Firedancer, and the C harness used
GCC 16.2.1 with `-O3 -march=native -fno-stack-protector`.  Base58 Turbo 0.3.0
and five8 1.0.0 used their Cargo release profiles with Rust 1.96.0 and
`-C target-cpu=native`.  Their AVX2 paths were enabled.  The Firedancer
comparison is official commit `e14b9929232019aa61f9258406a4c926e5fee75a`
with its AVX2 path.

Before timing, the harness compared Braid58, Base58 Turbo, five8, and
Firedancer encodings and round-trips for 1,024 deterministic 32-byte inputs.
It also compared Turbo, five8, and Firedancer for 1,024 64-byte inputs.  The
inputs have their high bit set so the timed decoder corpus consistently uses
the common maximum lengths of 44 and 88 characters.  All comparisons passed.

The values below come from 15 trials of 1,000,000 complete public API calls.
The corpus and output buffers are hot in cache.  TSC ticks are neither core
cycles nor nanoseconds; this host's invariant TSC calibrated to 2.500 GHz.
Calls/s and GiB/s use the median tick count.  Matching Base58 Turbo's Criterion
convention, encode throughput counts binary input bytes and decode throughput
counts encoded input bytes (44 or 88 per call).

| Operation | Min ticks | Median ticks | Max ticks | Mcalls/s | GiB/s |
|---|---:|---:|---:|---:|---:|
| Braid58 encode 32 | 35.18 | 35.55 | 38.42 | 70.33 | 2.096 |
| Base58 Turbo encode 32 | 52.60 | 54.01 | 58.81 | 46.28 | 1.379 |
| five8 encode 32 | 77.95 | 78.54 | 86.02 | 31.83 | 0.949 |
| Firedancer encode 32 | 68.44 | 68.81 | 74.58 | 36.33 | 1.083 |
| Braid58 decode 32 | 27.94 | 27.99 | 30.47 | 89.31 | 3.660 |
| Base58 Turbo decode 32 | 43.98 | 44.41 | 49.32 | 56.30 | 2.307 |
| five8 decode 32 | 61.71 | 62.70 | 70.67 | 39.87 | 1.634 |
| Firedancer decode 32 | 100.45 | 101.23 | 110.60 | 24.70 | 1.012 |
| Base58 Turbo encode 64 | 100.12 | 100.87 | 109.97 | 24.78 | 1.477 |
| five8 encode 64 | 127.57 | 128.85 | 140.48 | 19.40 | 1.156 |
| Firedancer encode 64 | 122.41 | 122.84 | 130.29 | 20.35 | 1.213 |
| Base58 Turbo decode 64 | 79.59 | 80.73 | 89.50 | 30.97 | 2.538 |
| five8 decode 64 | 253.12 | 255.60 | 282.63 | 9.78 | 0.802 |
| Firedancer decode 64 | 360.01 | 366.79 | 381.98 | 6.82 | 0.559 |

At the median, Braid58 used 34.2% fewer TSC ticks than Base58 Turbo for 32-byte
encoding and 37.0% fewer for decoding.  Against five8, the reductions were
54.7% and 55.4%, or 2.21x and 2.24x the call throughput.  Against Firedancer,
the reductions were 48.3% and 72.4%.  At 64 bytes Turbo was fastest for both
operations.  five8 encoding used 4.9% more ticks than Firedancer, while its
decoder used 30.3% fewer.  Braid58 has no 64-byte implementation, so those
rows are standalone baselines rather than direct comparisons.

These local absolute numbers are much lower than the EPYC record in
`DESIGN.md`, but that does not contradict it: invariant-TSC rate, boost state,
AVX-512 behavior, compiler, and microarchitecture all differ.  The bundled
correctness claims reproduced.  The original benchmark harness is absent from
the archive, so its exact EPYC ranges still cannot be regenerated.  This
same-host run does reproduce the claimed ordering against the exact Base58
Turbo 0.3.0 release, with a larger local margin, while adding five8 1.0.0 as a
second Rust baseline in a new documented harness.

Reproduce with:

```sh
make test
BENCH_CPU=31 make bench
```
