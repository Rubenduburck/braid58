#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
avx2_build="$root/build/audit-avx2"
avx512_build="$root/build/audit-avx512"

cmake -S "$root" -B "$avx2_build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=ON -DBRAID58_TARGET=avx2
cmake --build "$avx2_build" --parallel

cmake -S "$root" -B "$avx512_build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=ON -DBRAID58_TARGET=avx512
cmake --build "$avx512_build" --parallel

avx2_object_dir="$avx2_build/CMakeFiles/braid58_objects.dir/c"
test -n "$(find "$avx2_object_dir" -type f -name '*avx2*.o' -print -quit)"
if find "$avx2_object_dir" -type f -name '*avx2*.o' \
    -exec objdump -d {} + | \
    grep -Eiq 'zmm|%k[0-7]|vpmadd52|vpermb|vpermi2b|vpermt2b'; then
  echo "AVX-512 instruction found in AVX2 objects" >&2
  exit 1
fi

for object in "$avx512_build"/CMakeFiles/braid58_objects.dir/c/*avx512*.o; do
  objdump -d "$object" | grep -Eq 'zmm[0-9]' || {
    echo "missing ZMM code in $object" >&2
    exit 1
  }
done

encode64="$avx512_build/CMakeFiles/braid58_objects.dir/c/encode_avx512_64.c.o"
decode64="$avx512_build/CMakeFiles/braid58_objects.dir/c/decode_avx512_64.c.o"
if objdump -d "$encode64" | \
    grep -Eiq 'vpmullq|vpmadd52|vpermb|vpermi2b|vpermt2b'; then
  echo "rejected encode64 instruction family survived optimization" >&2
  exit 1
fi
if objdump -d "$decode64" | grep -Eiq 'vpmadd52|vpdpbusd'; then
  echo "rejected decode64 instruction family survived optimization" >&2
  exit 1
fi

if grep -R -n -E '^#[[:space:]]*include[[:space:]]+["<][^">]+\.c[">]' \
    "$root/c"; then
  echo "C source inclusion found" >&2
  exit 1
fi

expected='braid58_decode_32 braid58_decode_64 braid58_encode_32 braid58_encode_32x2 braid58_encode_32x3 braid58_encode_64 braid58_encode_64x2 braid58_encode_64x3 '
for library in "$avx2_build/libbraid58.so" "$avx512_build/libbraid58.so"; do
  exports=$(nm -D -g --defined-only "$library" |
    awk '$2 ~ /^[TDBR]$/ {print $3}' | sort | tr '\n' ' ')
  test "$exports" = "$expected" || {
    echo "unexpected public symbols in $library: $exports" >&2
    exit 1
  }
done

echo "audit: strict AVX2; selected ZMM kernels; exact public ABI"
