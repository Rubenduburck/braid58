# Local benchmark record

## 2026-08-15 comparison with Base58 Turbo, five8, and Firedancer

The repository benchmark was run on an AMD Ryzen Threadripper PRO 9995WX
96-Core, pinned to logical CPU 31.  Braid58 used its runtime-dispatched public
C API, compiled with GCC 16.2.1 at `-O3 -mtune=native`; its private AVX-512
and AVX2 functions carry explicit target attributes. Firedancer and the C
harness used
`-O3 -march=native -fno-stack-protector`.  Base58 Turbo 0.3.0 and five8 1.0.0
used their Cargo release profiles with Rust 1.96.0 and
`-C target-cpu=native`.  The Firedancer comparison is official commit
`e14b9929232019aa61f9258406a4c926e5fee75a` with its AVX2 path.

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
| Braid58 encode 32 | 34.34 | 34.79 | 37.44 | 71.87 | 2.142 |
| Base58 Turbo encode 32 | 53.49 | 54.47 | 58.26 | 45.90 | 1.368 |
| five8 encode 32 | 78.05 | 79.06 | 85.34 | 31.62 | 0.942 |
| Firedancer encode 32 | 68.52 | 69.44 | 74.75 | 36.00 | 1.073 |
| Braid58 decode 32 | 30.26 | 30.52 | 33.08 | 81.92 | 3.357 |
| Base58 Turbo decode 32 | 44.02 | 44.26 | 48.03 | 56.48 | 2.314 |
| five8 decode 32 | 62.61 | 64.43 | 68.64 | 38.80 | 1.590 |
| Firedancer decode 32 | 100.90 | 102.74 | 109.69 | 24.33 | 0.997 |
| Base58 Turbo encode 64 | 100.12 | 100.95 | 108.96 | 24.76 | 1.476 |
| five8 encode 64 | 119.03 | 120.45 | 131.16 | 20.75 | 1.237 |
| Firedancer encode 64 | 122.83 | 123.96 | 133.79 | 20.17 | 1.202 |
| Base58 Turbo decode 64 | 80.78 | 81.89 | 88.22 | 30.53 | 2.502 |
| five8 decode 64 | 254.87 | 257.79 | 275.03 | 9.70 | 0.795 |
| Firedancer decode 64 | 357.95 | 369.86 | 391.39 | 6.76 | 0.554 |

At the median, Braid58 used 36.1% fewer TSC ticks than Base58 Turbo for 32-byte
encoding and 31.0% fewer for decoding. Against five8, the reductions were
56.0% and 52.6%, or 2.27x and 2.11x the call throughput. Against Firedancer,
the reductions were 49.9% and 70.3%. At 64 bytes Turbo was fastest for both
operations. five8 used 2.8% fewer encoding ticks and 30.3% fewer decoding
ticks than Firedancer. Braid58 has no 64-byte implementation, so those rows
are standalone baselines rather than direct comparisons.

### AVX2-only Braid58

The same harness was rebuilt with `BENCH_BRAID_MAX_ISA=avx2`, which omits the
AVX-512 objects and makes the public dispatcher select the dedicated AVX2
backend. Competitor settings, inputs, pinning, throughput accounting, trial
ordering, and validation were unchanged. The final record below raises the
sample count to 31 trials of 1,000,000 calls.

| Operation | Min ticks | Median ticks | Max ticks | Mcalls/s | GiB/s |
|---|---:|---:|---:|---:|---:|
| Braid58 AVX2 encode 32 | 53.92 | 55.53 | 59.00 | 45.02 | 1.342 |
| Base58 Turbo encode 32 | 52.64 | 54.09 | 57.45 | 46.21 | 1.377 |
| five8 encode 32 | 78.77 | 79.72 | 85.12 | 31.36 | 0.935 |
| Firedancer encode 32 | 68.43 | 70.36 | 74.71 | 35.53 | 1.059 |
| Braid58 AVX2 decode 32 | 69.45 | 70.93 | 81.77 | 35.25 | 1.444 |
| Base58 Turbo decode 32 | 43.92 | 45.22 | 74.27 | 55.29 | 2.266 |
| five8 decode 32 | 61.72 | 63.51 | 93.21 | 39.36 | 1.613 |
| Firedancer decode 32 | 101.26 | 103.05 | 109.66 | 24.26 | 0.994 |

The radix-`58^5` AVX2 encoder used 30.3% fewer median ticks than five8 and
21.1% fewer than Firedancer, corresponding to 1.44x and 1.27x their call
throughput. Turbo used 2.6% fewer ticks than Braid58 in the 31-trial record.
Two preceding 15-trial runs put Braid58 and Turbo on opposite sides by 3.9%
and 3.3%, so this host supports a near-parity conclusion rather than a robust
ordering between those two. The AVX2 decoder remains slower than Turbo and
five8, but used 31.2% fewer ticks than Firedancer and delivered 1.45x its call
throughput. Native AVX-512 is still materially faster: the recorded AVX-512
encoder used 37.3% fewer median ticks than the AVX2 encoder. The algorithmic
advantage therefore carries to AVX2 against five8 and Firedancer without
erasing the additional AVX-512 benefit.

These local absolute numbers are much lower than the EPYC record in
`DESIGN.md`, but that does not contradict it: invariant-TSC rate, boost state,
AVX-512 behavior, compiler, and microarchitecture all differ.  The bundled
correctness claims reproduced.  The original benchmark harness is absent from
the archive, so its exact EPYC ranges still cannot be regenerated.  This
same-host run reproduces the claimed ordering against the exact Base58 Turbo
0.3.0 release while including both runtime dispatch and the complete public
API.  five8 1.0.0 remains a second Rust baseline in the documented harness.

Reproduce with:

```sh
make test
BENCH_CPU=31 make bench
BENCH_CPU=31 BENCH_TRIALS=31 BENCH_BRAID_MAX_ISA=avx2 make bench
```
