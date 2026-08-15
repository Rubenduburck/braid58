#!/usr/bin/env bash
set -euo pipefail

readonly FIREDANCER_COMMIT=e14b9929232019aa61f9258406a4c926e5fee75a
readonly FIREDANCER_URL=https://github.com/firedancer-io/firedancer.git
readonly BASE58_TURBO_VERSION=0.3.0

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${BUILD_DIR:-"${repo_dir}/build/bench"}
firedancer_dir=${FIREDANCER_DIR:-"${build_dir}/firedancer"}
compiler=${CC:-cc}
cargo_command=${CARGO:-cargo}
cargo_home=${CARGO_HOME:-"${build_dir}/cargo-home"}

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

CARGO_HOME="${cargo_home}" RUSTFLAGS="${RUSTFLAGS:-} -C target-cpu=native" \
  "${cargo_command}" build --release --locked \
  --manifest-path "${repo_dir}/bench/turbo_bridge/Cargo.toml" \
  --target-dir "${build_dir}/turbo-target"

common_flags=(
  -O3
  -march=native
  -fno-stack-protector
  -Wall
  -Wextra
  -Werror
)

"${compiler}" "${common_flags[@]}" -DBRAID58_NO_MAIN \
  -I"${repo_dir}/include" -c "${repo_dir}/src/encode_r6.c" \
  -o "${build_dir}/braid58_encode.o"
"${compiler}" "${common_flags[@]}" -I"${repo_dir}/include" \
  -c "${repo_dir}/src/decode_r6.c" \
  -o "${build_dir}/braid58_decode.o"
"${compiler}" "${common_flags[@]}" -DFD_HAS_AVX=1 -DFD_HAS_SSE=1 \
  -Dfd_base58_encode_32=firedancer_base58_encode_32 \
  -Dfd_base58_encode_64=firedancer_base58_encode_64 \
  -Dfd_base58_decode_32=firedancer_base58_decode_32 \
  -Dfd_base58_decode_64=firedancer_base58_decode_64 \
  -I"${firedancer_dir}/src" \
  -c "${firedancer_dir}/src/ballet/base58/fd_base58.c" \
  -o "${build_dir}/firedancer_base58.o"
"${compiler}" "${common_flags[@]}" -I"${repo_dir}/include" \
  "${repo_dir}/bench/bench_base58.c" \
  "${build_dir}/braid58_encode.o" \
  "${build_dir}/braid58_decode.o" \
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
echo

exec taskset -c "${cpu}" "${build_dir}/bench_base58" \
  "${iterations}" "${trials}"
