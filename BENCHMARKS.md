# Local benchmark record

## 2026-08-15 comparison with Firedancer

The repository benchmark was run on an AMD Ryzen Threadripper PRO 9995WX
96-Core, pinned to logical CPU 31, with GCC 16.2.1.  All objects used
`-O3 -march=native -fno-stack-protector`.  The Firedancer comparison is
official commit `e14b9929232019aa61f9258406a4c926e5fee75a` with its AVX2 path
enabled.

Before timing, the harness compared Braid58 and Firedancer encodings and
round-trips for 1,024 deterministic 32-byte inputs.  It separately verified
1,024 Firedancer 64-byte round-trips.  The inputs have their high bit set so
the timed decoder corpus consistently uses the common maximum lengths of 44
and 88 characters.  All comparisons passed.

The values below are unadjusted invariant-TSC ticks per complete public API
call from 15 trials of 1,000,000 calls.  The corpus and output buffers are hot
in cache.  TSC ticks are neither core cycles nor nanoseconds.

| Operation | Minimum | Median | Maximum |
|---|---:|---:|---:|
| Braid58 encode 32 | 33.94 | 34.93 | 37.08 |
| Firedancer encode 32 | 68.36 | 69.68 | 74.57 |
| Braid58 decode 32 | 27.77 | 27.85 | 29.54 |
| Firedancer decode 32 | 102.13 | 103.07 | 111.32 |
| Firedancer encode 64 | 121.76 | 125.36 | 133.17 |
| Firedancer decode 64 | 353.24 | 362.21 | 375.68 |

At the median, Braid58 used 49.9% fewer TSC ticks for 32-byte encoding and
73.0% fewer for 32-byte decoding than Firedancer on this host.  Braid58 has no
64-byte implementation, so the Firedancer 64-byte rows are standalone
baselines rather than direct comparisons.

These local absolute numbers are much lower than the EPYC record in
`DESIGN.md`, but that does not contradict it: invariant-TSC rate, boost state,
AVX-512 behavior, compiler, and microarchitecture all differ.  The bundled
correctness claims reproduced.  The old Base58 Turbo performance comparison
did not, because its source and original harness are absent from the archive;
this run instead performs the newly requested same-host Firedancer comparison.

Reproduce with:

```sh
make test
BENCH_CPU=31 make bench
```
