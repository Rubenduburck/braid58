# Benchmarks

Benchmark reproduction requires a full repository checkout.

## Host

- CPU: AMD Ryzen Threadripper PRO 9995WX
- CPU affinity: logical CPU 31
- C compiler: GCC 16.2.1
- Rust: 1.96.0
- TSC: invariant, 2.500 GHz calibration
- Corpus: deterministic, cache-resident

TSC ticks are not core cycles. GiB/s counts binary input bytes for encode and
encoded input bytes for decode.

## Scalar comparison

Base58 Turbo was built without its default `unsafe-simd` feature. Both codec
paths target the x86-64 ISA baseline; Braid58 is tuned for the benchmark host
without enabling a higher ISA. The corpus and output validation are otherwise
the same as the main benchmark.

| Operation | Braid ticks | Turbo ticks | Braid GiB/s | Turbo GiB/s | Braid difference |
|---|---:|---:|---:|---:|---:|
| Encode32 | 59.13 | 57.65 | 1.260 | 1.292 | +2.6% |
| Decode32 | 53.53 | 56.55 | 1.914 | 1.812 | -5.3% |
| Encode64 | 173.35 | 195.32 | 0.860 | 0.763 | -11.2% |
| Decode64 | 117.21 | 123.16 | 1.748 | 1.664 | -4.8% |

Fifteen trials of one million calls were pinned to logical CPU 31. Run with:

```sh
BENCH_BRAID_TARGET=scalar \
BENCH_TURBO_BACKEND=scalar \
BENCH_TURBO_TARGET_CPU=x86-64 \
BENCH_CPU=31 BENCH_ITERATIONS=1000000 BENCH_TRIALS=15 \
  ./bench/run.sh
```

## Base58 Turbo gate

Comparator:

- Base58 Turbo 0.3.0
- Commit `18c8f94eadfa5643dfd7e31b02250d3bf184fa68`
- Features `unsafe-simd,std`
- `RUSTFLAGS=-C target-cpu=znver5`

The gate uses one executable, verifies outputs before timing, alternates call
order, and reports the median of 17 paired trials.

| Operation | Corpus | Braid ticks | Turbo ticks | Braid reduction |
|---|---|---:|---:|---:|
| Encode32 | full width | 31.618 | 54.642 | 41.348% |
| Decode32 | full width | 25.448 | 49.144 | 48.010% |
| Encode64 | full width | 90.627 | 104.281 | 13.227% |
| Decode64 | full width | 28.596 | 86.777 | 67.081% |
| Encode32 | leading-zero rotation | 33.993 | 60.487 | 43.214% |
| Decode32 | leading-zero rotation | 27.845 | 45.383 | 38.279% |
| Encode64 | leading-zero rotation | 91.439 | 106.730 | 13.929% |
| Decode64 | leading-zero rotation | 29.370 | 78.169 | 62.418% |

Result: `gate=PASS failures=0`.

Comparator build and gate invocation are specified in
[docs/TURBO_GATE.md](docs/TURBO_GATE.md).

## Base58 Turbo Criterion benchmark

This run uses Base58 Turbo's `benches/encoding_bench.rs` and
`benches/scripts/plot_bench.py` at commit
`18c8f94eadfa5643dfd7e31b02250d3bf184fa68`. The applied
[patch](bench/turbo-criterion.patch) adds Braid58 Encode32, Decode32, Encode64,
and Decode64 entries and the corresponding plot series. It compares Braid58
output with Turbo before each timed entry. Turbo's original Criterion
configuration remains 3 seconds of warm-up, 5 seconds of measurement, 50
samples, and a 5% noise threshold.

The original harness generates a new unseeded value for each size on every
run. The patch replaces that generator with `Xoshiro256PlusPlus`, seed
`0x4252414944353800 ^ size`, and forces the most-significant byte nonzero. This
makes the two backend runs use identical full-width values and avoids measuring
Turbo's leading-zero special case by accident.

AVX2 and AVX-512 are linked into separate benchmark executables. Base58 Turbo
uses its `unsafe-simd` AVX2 path under `-C target-cpu=native` and has no AVX-512
kernel. Each Braid result below is paired with the Turbo result from the same
executable. Both processes were pinned to logical CPU 31.

> **ISA comparison:** The AVX-512 rows compare Braid58 AVX-512 with Base58
> Turbo AVX2. The AVX2 rows measure both implementations at the same ISA
> level.

| Executable | Operation | Braid time | Braid throughput | Turbo time | Turbo throughput |
|---|---|---:|---:|---:|---:|
| AVX2 | Encode32 | 22.306 ns | 1.3361 GiB/s | 23.942 ns | 1.2448 GiB/s |
| AVX2 | Decode32 | 11.087 ns | 3.6961 GiB/s | 17.196 ns | 2.3830 GiB/s |
| AVX2 | Encode64 | 41.492 ns | 1.4365 GiB/s | 43.813 ns | 1.3604 GiB/s |
| AVX2 | Decode64 | 28.164 ns | 2.9100 GiB/s | 36.103 ns | 2.2701 GiB/s |
| AVX-512 | Encode32 | 10.914 ns | 2.7307 GiB/s | 21.621 ns | 1.3784 GiB/s |
| AVX-512 | Decode32 | 8.2007 ns | 4.9969 GiB/s | 17.507 ns | 2.3406 GiB/s |
| AVX-512 | Encode64 | 36.217 ns | 1.6458 GiB/s | 41.661 ns | 1.4307 GiB/s |
| AVX-512 | Decode64 | 11.238 ns | 7.2930 GiB/s | 33.860 ns | 2.4205 GiB/s |

The README charts retain Turbo's original 16/32/48/64/128-byte sweep and
comparators. Braid58 appears only at 32 and 64 bytes. Encode throughput counts
binary input bytes; decode throughput counts encoded Base58 bytes.

## Fixed x3 encoding

The native benchmark also compares `braid58_encode_32x3` with Base58 Turbo's
public `encode_32_batch` using exactly three inputs per call. Both write one
length per lane; Braid also writes a trailing NUL. The deterministic corpus
contains full-width 44-character values. Outputs are compared before timing.
Each row is the median of 15 trials of one million batch calls on CPU 31.
The harness makes one C ABI call per batch. Turbo's bridge constructs fixed
slices and calls the public Rust method; it does not change Turbo's algorithm.

Turbo is built with `-C target-cpu=haswell` for these rows. The AVX2 row is
therefore an equal-ISA comparison and both AVX2 objects are audited to contain
no AVX-512 instructions.

| Braid target | Braid ticks/batch | Braid ticks/value | Braid GiB/s | Turbo ticks/batch | Turbo ticks/value | Turbo GiB/s | Braid time reduction |
|---|---:|---:|---:|---:|---:|---:|---:|
| AVX2 | 117.68 | 39.23 | 1.899 | 140.03 | 46.68 | 1.596 | 16.0% |
| AVX-512 | 66.98 | 22.33 | 3.337 | 144.94 | 48.31 | 1.542 | 53.8% |

| Braid target | Single ticks/value | x3 ticks/value | x3 reduction |
|---|---:|---:|---:|
| AVX2 | 56.95 | 39.23 | 31.1% |
| AVX-512 | 28.03 | 22.33 | 20.3% |

Encode64 has no Turbo batch API, so the comparator below is three sequential
Turbo `encode_into` calls. Braid AVX2 uses its dedicated x3 schedule. Braid
AVX-512 uses three selected single calls because no dedicated AVX-512
Encode64 batch kernel is included.

| Braid target | Braid ticks/batch | Braid ticks/value | Braid GiB/s | Turbo ticks/batch | Turbo ticks/value | Turbo GiB/s |
|---|---:|---:|---:|---:|---:|---:|
| AVX2 | 248.67 | 82.89 | 1.798 | 279.05 | 93.02 | 1.602 |
| AVX-512 | 263.31 | 87.77 | 1.698 | 289.47 | 96.49 | 1.544 |

The AVX2 Encode64 x3 schedule reduces Braid's own time per value by 20.4%
relative to its 104.19-tick single row in the same executable.

> **ISA comparison:** The AVX-512 rows compare Braid58 AVX-512 with Turbo AVX2.
> AVX-512 activity in the shared process may also affect core frequency. The
> AVX2 rows measure both implementations at the same ISA level.

Run the equal-ISA AVX2 batch benchmark with:

```sh
BENCH_BRAID_TARGET=avx2 \
BENCH_TURBO_TARGET_CPU=haswell \
BENCH_AVX2_TUNE=haswell \
BENCH_CPU=31 BENCH_ITERATIONS=1000000 BENCH_TRIALS=15 \
  ./bench/run.sh
```

For the cross-ISA row, change `BENCH_BRAID_TARGET` to `avx512` and leave
Turbo at `haswell`.

Base58 Turbo Criterion reproduction:

```sh
BRAID58_DIR=$PWD

cmake -S "$BRAID58_DIR" -B /tmp/braid58-criterion-avx2 \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBRAID58_TARGET=avx2 \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=znver5"
cmake --build /tmp/braid58-criterion-avx2 --parallel

cmake -S "$BRAID58_DIR" -B /tmp/braid58-criterion-avx512 \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBRAID58_TARGET=avx512 \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=znver5"
cmake --build /tmp/braid58-criterion-avx512 --parallel

git clone https://github.com/hacer-bark/base58-turbo.git /tmp/base58-turbo
git -C /tmp/base58-turbo checkout 18c8f94eadfa5643dfd7e31b02250d3bf184fa68
git -C /tmp/base58-turbo apply "$BRAID58_DIR/bench/turbo-criterion.patch"

mkdir -p /tmp/base58-turbo/native
cp /tmp/braid58-criterion-avx2/libbraid58.a \
  /tmp/base58-turbo/native/libbraid58.a

cd /tmp/base58-turbo
taskset -c 31 env \
  CARGO_TARGET_DIR=/tmp/base58-turbo-target-avx2 \
  BRAID58_BENCH_BACKEND=AVX2 \
  RUSTFLAGS="-C target-cpu=native -L native=/tmp/base58-turbo/native" \
  BENCH_TARGET=all cargo bench --bench encoding_bench \
  2>&1 | tee /tmp/braid58-turbo-criterion-avx2.txt

python3 /tmp/base58-turbo/benches/scripts/plot_bench.py \
  /tmp/braid58-turbo-criterion-avx2.txt --braid-backend avx2 \
  --out "$BRAID58_DIR/bench/results/turbo-criterion-avx2-9995wx.png"

cp /tmp/braid58-criterion-avx512/libbraid58.a \
  /tmp/base58-turbo/native/libbraid58.a

taskset -c 31 env \
  CARGO_TARGET_DIR=/tmp/base58-turbo-target-avx512 \
  BRAID58_BENCH_BACKEND=AVX512 \
  RUSTFLAGS="-C target-cpu=native -L native=/tmp/base58-turbo/native" \
  BENCH_TARGET=all cargo bench --bench encoding_bench \
  2>&1 | tee /tmp/braid58-turbo-criterion-avx512.txt

python3 /tmp/base58-turbo/benches/scripts/plot_bench.py \
  /tmp/braid58-turbo-criterion-avx512.txt --braid-backend avx512 \
  --out "$BRAID58_DIR/bench/results/turbo-criterion-avx512-9995wx.png"
```

## Braid58 throughput

Fifteen trials, one million calls per trial:

| Target | Operation | Median ticks | Mcalls/s | GiB/s |
|---|---|---:|---:|---:|
| AVX-512 | Encode32 | 26.49 | 94.38 | 2.813 |
| AVX-512 | Decode32 | 20.17 | 123.94 | 5.079 |
| AVX-512 | Encode64 | 83.54 | 29.93 | 1.784 |
| AVX-512 | Decode64 | 27.48 | 90.99 | 7.457 |
| AVX2 | Encode32 | 54.91 | 45.52 | 1.357 |
| AVX2 | Decode32 | 29.05 | 86.04 | 3.526 |
| AVX2 | Encode64 | 103.07 | 24.26 | 1.446 |
| AVX2 | Decode64 | 67.89 | 36.82 | 3.018 |

AVX2 versus Turbo median ticks in the same run:

| Operation | Braid | Turbo | Difference |
|---|---:|---:|---:|
| Encode32 | 54.91 | 55.38 | -0.85% |
| Decode32 | 29.05 | 47.53 | -38.88% |
| Encode64 | 103.07 | 103.24 | -0.16% |
| Decode64 | 67.89 | 81.38 | -16.58% |

Encode differences below 1% are within observed run-to-run variance.

## Commands

```sh
make test-optimized TEST_TARGET=native
make audit
BENCH_CPU=31 BENCH_TRIALS=15 make bench
```

`bench/run.sh` prints the resolved Braid target and Turbo target CPU. Set both
`BENCH_TURBO_TARGET_CPU=haswell` and `BENCH_AVX2_TUNE=haswell` for the strict
equal-ISA batch comparison above.

`make bench` compares Braid58 with Base58 Turbo 0.3.0, five8 1.0.0, and
Firedancer commit `e14b9929232019aa61f9258406a4c926e5fee75a` after validating
the shared 32-byte and 64-byte inputs.
