#!/usr/bin/env bash
set -euo pipefail

readonly FIREDANCER_COMMIT=e14b9929232019aa61f9258406a4c926e5fee75a
readonly FIREDANCER_URL=https://github.com/firedancer-io/firedancer.git
readonly BASE58_TURBO_VERSION=0.3.0
readonly FIVE8_VERSION=1.0.0

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${BUILD_DIR:-"${repo_dir}/build/bench"}
firedancer_dir=${FIREDANCER_DIR:-"${build_dir}/firedancer"}
compiler=${CC:-cc}
cargo_command=${CARGO:-cargo}
cargo_home=${CARGO_HOME:-"${build_dir}/cargo-home"}
braid_target=${BENCH_BRAID_TARGET:-native}
turbo_target_cpu=${BENCH_TURBO_TARGET_CPU:-native}
turbo_backend=${BENCH_TURBO_BACKEND:-simd}
avx2_tune=${BENCH_AVX2_TUNE:-native}

case "${turbo_backend}" in
  simd)
    turbo_features=(--features turbo-simd)
    ;;
  scalar)
    turbo_features=(--no-default-features --features turbo-scalar)
    ;;
  *)
    echo "BENCH_TURBO_BACKEND must be simd or scalar" >&2
    exit 2
    ;;
esac

mkdir -p "${build_dir}"

if [[ ! -f "${firedancer_dir}/src/ballet/base58/fd_base58.c" ]]; then
  git clone --depth 1 --filter=blob:none --sparse \
    "${FIREDANCER_URL}" "${firedancer_dir}"
  git -C "${firedancer_dir}" sparse-checkout set src/ballet/base58 src/util
  git -C "${firedancer_dir}" fetch --depth 1 origin "${FIREDANCER_COMMIT}"
  git -C "${firedancer_dir}" checkout --detach "${FIREDANCER_COMMIT}"
fi

actual_commit=$(git -C "${firedancer_dir}" rev-parse HEAD)
if [[ "${actual_commit}" != "${FIREDANCER_COMMIT}" ]]; then
  echo "warning: requested Firedancer commit ${FIREDANCER_COMMIT}" >&2
  echo "warning: using Firedancer commit     ${actual_commit}" >&2
fi

CARGO_HOME="${cargo_home}" \
  RUSTFLAGS="${RUSTFLAGS:-} -C target-cpu=${turbo_target_cpu}" \
  "${cargo_command}" build --release --locked \
  "${turbo_features[@]}" \
  --manifest-path "${repo_dir}/bench/turbo_bridge/Cargo.toml" \
  --target-dir "${build_dir}/turbo-target"

if [[ "${turbo_target_cpu}" == "haswell" ]] &&
   objdump -d "${build_dir}/turbo-target/release/libbase58_turbo_bridge.a" |
   grep -Ei 'zmm[0-9]|%k[0-7]' >/dev/null; then
  echo "AVX-512 instruction found in Haswell-targeted Turbo archive" >&2
  exit 1
fi

portable_flags=(
  -O3
  -std=c17
  -fno-stack-protector
  -Wall
  -Wextra
  -Werror
)
native_flags=("${portable_flags[@]}" -march=native)

case "${braid_target}" in
  native)
    native_defines=$("${compiler}" -march=native -dM -E -x c /dev/null)
    if [[ "${native_defines}" == *"__AVX2__ 1"* &&
          "${native_defines}" == *"__BMI__ 1"* &&
          "${native_defines}" == *"__MOVBE__ 1"* &&
          "${native_defines}" == *"__AVX512F__ 1"* &&
          "${native_defines}" == *"__AVX512DQ__ 1"* &&
          "${native_defines}" == *"__AVX512BW__ 1"* &&
          "${native_defines}" == *"__AVX512VL__ 1"* &&
          "${native_defines}" == *"__AVX512IFMA__ 1"* &&
          "${native_defines}" == *"__AVX512VBMI__ 1"* ]]; then
      braid_resolved_target=avx512
    elif [[ "${native_defines}" == *"__AVX2__ 1"* ]]; then
      braid_resolved_target=avx2
    else
      braid_resolved_target=scalar
    fi
    ;;
  avx2)
    braid_resolved_target=avx2
    ;;
  avx512)
    braid_resolved_target=avx512
    ;;
  scalar)
    braid_resolved_target=scalar
    ;;
  *)
    echo "BENCH_BRAID_TARGET must be native, avx512, avx2, or scalar" >&2
    exit 2
    ;;
esac

case "${braid_resolved_target}" in
  avx512)
    braid_target_value=2
    braid_avx2=0
    braid_avx512=1
    ;;
  avx2)
    braid_target_value=1
    braid_avx2=1
    braid_avx512=0
    ;;
  scalar)
    braid_target_value=0
    braid_avx2=0
    braid_avx512=0
    ;;
esac

"${compiler}" "${portable_flags[@]}" -mtune=native \
  -DBRAID58_BUILDING_LIBRARY \
  -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
  -I"${repo_dir}/include" -I"${repo_dir}/c" \
  -c "${repo_dir}/c/braid58.c" \
  -o "${build_dir}/braid58.o"
braid_objects=("${build_dir}/braid58.o")
if (( braid_avx2 )); then
  "${compiler}" "${portable_flags[@]}" -march=haswell -mno-avx512f \
    -mtune="${avx2_tune}" \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/encode_avx2.c" \
    -o "${build_dir}/braid58_encode_avx2.o"
  "${compiler}" "${portable_flags[@]}" -march=haswell -mno-avx512f \
    -mtune="${avx2_tune}" \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/decode_avx2.c" \
    -o "${build_dir}/braid58_decode_avx2.o"
  "${compiler}" "${portable_flags[@]}" -march=haswell -mno-avx512f \
    -mtune="${avx2_tune}" \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/encode_avx2_64.c" \
    -o "${build_dir}/braid58_encode_avx2_64.o"
  "${compiler}" "${portable_flags[@]}" -march=haswell -mno-avx512f \
    -mtune="${avx2_tune}" \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/decode_avx2_64.c" \
    -o "${build_dir}/braid58_decode_avx2_64.o"
  braid_objects+=("${build_dir}/braid58_encode_avx2.o"
                   "${build_dir}/braid58_decode_avx2.o"
                   "${build_dir}/braid58_encode_avx2_64.o"
                   "${build_dir}/braid58_decode_avx2_64.o")
fi
if (( braid_avx512 )); then
  avx512_flags=(
    -mavx2
    -mbmi
    -mmovbe
    -mavx512f
    -mavx512dq
    -mavx512bw
    -mavx512vl
    -mavx512ifma
    -mavx512vbmi
  )
  "${compiler}" "${portable_flags[@]}" "${avx512_flags[@]}" -mtune=native \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/encode_avx512.c" \
    -o "${build_dir}/braid58_encode.o"
  "${compiler}" "${portable_flags[@]}" "${avx512_flags[@]}" -mtune=native \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/decode_avx512.c" \
    -o "${build_dir}/braid58_decode.o"
  "${compiler}" "${portable_flags[@]}" "${avx512_flags[@]}" -mtune=native \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/encode_avx512_64.c" \
    -o "${build_dir}/braid58_encode_64.o"
  "${compiler}" "${portable_flags[@]}" "${avx512_flags[@]}" -mtune=native \
    -DBRAID58_COMPILED_TARGET="${braid_target_value}" \
    -I"${repo_dir}/include" -I"${repo_dir}/c" \
    -c "${repo_dir}/c/decode_avx512_64.c" \
    -o "${build_dir}/braid58_decode_64.o"
  braid_objects+=("${build_dir}/braid58_encode.o"
                   "${build_dir}/braid58_decode.o"
                   "${build_dir}/braid58_encode_64.o"
                   "${build_dir}/braid58_decode_64.o")
fi
"${compiler}" "${native_flags[@]}" -DFD_HAS_AVX=1 -DFD_HAS_SSE=1 \
  -Dfd_base58_encode_32=firedancer_base58_encode_32 \
  -Dfd_base58_encode_64=firedancer_base58_encode_64 \
  -Dfd_base58_decode_32=firedancer_base58_decode_32 \
  -Dfd_base58_decode_64=firedancer_base58_decode_64 \
  -I"${firedancer_dir}/src" \
  -c "${firedancer_dir}/src/ballet/base58/fd_base58.c" \
  -o "${build_dir}/firedancer_base58.o"
"${compiler}" "${native_flags[@]}" \
  "-DBRAID58_BENCH_TARGET=\"${braid_resolved_target}\"" \
  -I"${repo_dir}/include" \
  "${repo_dir}/bench/bench_base58.c" \
  "${braid_objects[@]}" \
  "${build_dir}/firedancer_base58.o" \
  "${build_dir}/turbo-target/release/libbase58_turbo_bridge.a" \
  -ldl -lpthread -lm \
  -o "${build_dir}/bench_base58"

allowed_cpus=$(awk '/Cpus_allowed_list/ { print $2 }' /proc/self/status)
first_cpu_group=${allowed_cpus%%,*}
cpu=${BENCH_CPU:-${first_cpu_group##*-}}
iterations=${BENCH_ITERATIONS:-1000000}
trials=${BENCH_TRIALS:-15}

echo "CPU: $(lscpu | awk -F: '/Model name/ { sub(/^[[:space:]]+/, "", $2); print $2; exit }')"
echo "Pinned logical CPU: ${cpu} (allowed: ${allowed_cpus})"
echo "Compiler: $("${compiler}" --version | head -n 1)"
echo "Rust: $(rustc --version)"
echo "Firedancer: ${actual_commit}"
echo "Base58 Turbo: ${BASE58_TURBO_VERSION} (crates.io)"
echo "Base58 Turbo backend: ${turbo_backend}"
echo "Base58 Turbo target CPU: ${turbo_target_cpu}"
echo "five8: ${FIVE8_VERSION} (crates.io)"
echo "Braid58 target: ${braid_resolved_target} (requested: ${braid_target})"
echo

exec taskset -c "${cpu}" "${build_dir}/bench_base58" \
  "${iterations}" "${trials}"
