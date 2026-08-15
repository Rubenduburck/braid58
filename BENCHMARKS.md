# Local benchmark record

## 2026-08-15 comparison with Base58 Turbo and Firedancer

The repository benchmark was run on an AMD Ryzen Threadripper PRO 9995WX
96-Core, pinned to logical CPU 31.  Braid58, Firedancer, and the C harness used
GCC 16.2.1 with `-O3 -march=native -fno-stack-protector`.  Base58 Turbo 0.3.0
used its Cargo release profile with Rust 1.96.0 and `-C target-cpu=native`.
Turbo's default AVX2 feature was enabled.  The Firedancer comparison is
official commit `e14b9929232019aa61f9258406a4c926e5fee75a` with its AVX2 path.

Before timing, the harness compared Braid58, Base58 Turbo, and Firedancer
encodings and round-trips for 1,024 deterministic 32-byte inputs.  It also
compared Turbo and Firedancer for 1,024 64-byte inputs.  The inputs have their
high bit set so the timed decoder corpus consistently uses the common maximum
lengths of 44 and 88 characters.  All comparisons passed.

The values below come from 15 trials of 1,000,000 complete public API calls.
The corpus and output buffers are hot in cache.  TSC ticks are neither core
cycles nor nanoseconds; this host's invariant TSC calibrated to 2.500 GHz.
Calls/s and GiB/s use the median tick count.  Matching Base58 Turbo's Criterion
convention, encode throughput counts binary input bytes and decode throughput
counts encoded input bytes (44 or 88 per call).

| Operation | Min ticks | Median ticks | Max ticks | Mcalls/s | GiB/s |
|---|---:|---:|---:|---:|---:|
| Braid58 encode 32 | 34.00 | 35.59 | 37.11 | 70.24 | 2.093 |
| Base58 Turbo encode 32 | 53.14 | 54.54 | 57.99 | 45.83 | 1.366 |
| Firedancer encode 32 | 68.97 | 72.33 | 74.73 | 34.56 | 1.030 |
| Braid58 decode 32 | 27.96 | 30.27 | 30.62 | 82.59 | 3.384 |
| Base58 Turbo decode 32 | 44.07 | 46.30 | 48.25 | 54.00 | 2.213 |
| Firedancer decode 32 | 101.40 | 104.62 | 109.63 | 23.89 | 0.979 |
| Base58 Turbo encode 64 | 100.56 | 107.84 | 109.02 | 23.18 | 1.382 |
| Firedancer encode 64 | 123.07 | 132.99 | 133.89 | 18.80 | 1.120 |
| Base58 Turbo decode 64 | 79.82 | 83.08 | 87.28 | 30.09 | 2.466 |
| Firedancer decode 64 | 357.56 | 372.24 | 387.31 | 6.72 | 0.550 |

At the median, Braid58 used 34.7% fewer TSC ticks than Base58 Turbo for 32-byte
encoding and 34.6% fewer for decoding.  Against Firedancer, the reductions were
50.8% and 71.1%.  At 64 bytes, Turbo used 18.9% fewer ticks than Firedancer for
encoding and 77.7% fewer for decoding.  Braid58 has no 64-byte implementation,
so those rows are standalone baselines rather than direct comparisons.

These local absolute numbers are much lower than the EPYC record in
`DESIGN.md`, but that does not contradict it: invariant-TSC rate, boost state,
AVX-512 behavior, compiler, and microarchitecture all differ.  The bundled
correctness claims reproduced.  The original benchmark harness is absent from
the archive, so its exact EPYC ranges still cannot be regenerated.  This
same-host run does reproduce the claimed ordering against the exact Base58
Turbo 0.3.0 release, with a larger local margin, while using a new documented
harness.

Reproduce with:

```sh
make test
BENCH_CPU=31 make bench
```
