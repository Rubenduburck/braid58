BUILD_DIR ?= build/cmake
BUILD_TYPE ?= Release
CMAKE_ARGS ?=
TEST_TARGET ?= native

CC ?= cc
TURBO_COMMIT := 18c8f94eadfa5643dfd7e31b02250d3bf184fa68

.PHONY: all configure test test-optimized test-sanitize rust-test bench \
	install audit check-turbo-pin turbo-gate clean

all: configure
	cmake --build "$(BUILD_DIR)" --parallel

configure:
	cmake -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DBUILD_TESTING=ON $(CMAKE_ARGS)

test: all
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

test-optimized:
	cmake -S . -B build/test-optimized -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_TESTING=ON -DBRAID58_TARGET="$(TEST_TARGET)"
	cmake --build build/test-optimized --parallel
	ctest --test-dir build/test-optimized --output-on-failure

test-sanitize:
	cmake -S . -B build/test-sanitize -DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON -DBRAID58_TARGET="$(TEST_TARGET)" \
		-DCMAKE_C_FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
	cmake --build build/test-sanitize --parallel
	ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/test-sanitize \
		--output-on-failure

rust-test:
	cargo test
	RUSTFLAGS="-C target-cpu=native" cargo test

bench:
	./bench/run.sh

audit:
	./scripts/audit-isa.sh

check-turbo-pin:
	@test -n "$(TURBO_DIR)" || \
		{ echo "set TURBO_DIR to the pinned Base58 Turbo checkout"; exit 2; }
	@test "$$(git -C "$(TURBO_DIR)" rev-parse HEAD)" = "$(TURBO_COMMIT)"
	@git -C "$(TURBO_DIR)" diff --exit-code -- \
		src/simd.rs src/encode.rs src/decode.rs
	@echo "Turbo pin/algorithm-body gate passed: $(TURBO_COMMIT)"

turbo-gate:
	@$(MAKE) --no-print-directory check-turbo-pin TURBO_DIR="$(TURBO_DIR)"
	@test -n "$(TURBO_LIB)" || \
		{ echo "set TURBO_LIB to the patched Turbo static archive"; exit 2; }
	@test -f "$(TURBO_LIB)" || \
		{ echo "Turbo archive not found: $(TURBO_LIB)"; exit 2; }
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_TESTING=OFF -DBRAID58_TARGET=avx512
	cmake --build "$(BUILD_DIR)" --parallel
	$(CC) -std=c11 -O3 -Wall -Wextra -Werror -Iinclude \
		bench/turbo_gate.c "$(BUILD_DIR)/libbraid58.a" "$(TURBO_LIB)" \
		-lpthread -ldl -lm -lrt -lutil -o "$(BUILD_DIR)/turbo_gate"
	"$(BUILD_DIR)/turbo_gate"

install: all
	cmake --install "$(BUILD_DIR)"

clean:
	cmake -E remove_directory build
	cargo clean
