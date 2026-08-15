# Braid58

Fast, fixed-width Bitcoin Base58 for 32-byte values.

Braid58 provides a small C ABI and an allocation-free Rust API. It selects an
AVX-512 radix-conversion kernel at runtime on supported x86-64 CPUs and falls
back to a portable scalar implementation everywhere else. Encoding preserves
Bitcoin's leading-zero rule; decoding accepts only canonical values that
produce exactly 32 bytes.

## Rust

The crate is `no_std` and has no runtime Rust dependencies.

```rust
let bytes = [42_u8; 32];
let encoded = braid58::encode_32(&bytes);

assert_eq!(braid58::decode_32(encoded.as_str())?, bytes);
println!("{encoded}");
# Ok::<(), braid58::DecodeError>(())
```

`Encoded32` implements `Display`, `Deref<Target = str>`, `AsRef<str>`, and
`AsRef<[u8]>`. `decode_32` accepts either text or bytes, while
`decode_32_into` reuses a caller-provided output buffer and leaves it unchanged
on failure. `backend()` reports the selected implementation.

```sh
cargo test
cargo test --features force-scalar
```

The crate currently has `publish = false` because the imported prototype did
not include a project license. Choose a license before publishing it.

## C

```c
#include <braid58.h>

uint8_t input[BRAID58_BINARY_32_SIZE] = {0};
uint8_t decoded[BRAID58_BINARY_32_SIZE];
char encoded[BRAID58_ENCODED_32_CAPACITY];

size_t length = braid58_encode_32(input, encoded);
if (!braid58_decode_32(encoded, length, decoded)) {
  /* invalid, overflowing, or noncanonical input */
}
```

Build and install a static library by default, or set `BUILD_SHARED_LIBS=ON`:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake
cmake --install build/cmake --prefix /usr/local
```

Installed consumers can use either CMake or pkg-config:

```cmake
find_package(braid58 0.1 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE braid58::braid58)
```

```sh
cc app.c $(pkg-config --cflags --libs braid58)
```

The installed ABI exports only:

- `braid58_encode_32`
- `braid58_decode_32`
- `braid58_get_backend`

## CPU backends

The optimized backend requires AVX2 and AVX-512 F, DQ, BW, VL, IFMA, VBMI,
and VBMI2. The public API checks these features before dispatching, so binaries
remain safe on unsupported CPUs. Disable the optimized backend with
`-DBRAID58_ENABLE_AVX512=OFF` in CMake or the Rust `force-scalar` feature.

GNU-compatible and Clang-compatible compilers build the AVX-512 backend on
x86-64. Other compilers and architectures build the scalar backend only.

## Development

```sh
make test        # CMake build plus the C differential suite
make rust-test   # Rust unit tests and doctests
make bench       # Braid58, Base58 Turbo, five8, and Firedancer
```

The C suite covers two million encoder differentials, 200,000 arbitrary
decoder differentials, leading-zero cases, invalid bytes, overflow,
canonicality, failure atomicity, and both runtime backends. The Rust tests
exercise the safe wrapper and forced-scalar build.

On the recorded Threadripper PRO 9995WX run, the runtime-dispatched public C
API reached 2.142 GiB/s for 32-byte encoding and 3.357 GiB/s for 44-character
decoding. These are hot-cache throughput results, not universal latency claims.
See [BENCHMARKS.md](BENCHMARKS.md) for the complete same-host comparison and
[DESIGN.md](DESIGN.md) for the radix construction.

## Scope

- Exactly 32 decoded bytes and the Bitcoin alphabet.
- No allocation in either public API.
- Decoder failure leaves the output untouched.
- Research implementation: not audited and not promised constant-time.
