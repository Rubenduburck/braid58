BUILD_DIR ?= build/cmake
BUILD_TYPE ?= Release
CMAKE_ARGS ?=

.PHONY: all configure test rust-test bench install clean

all: configure
	cmake --build "$(BUILD_DIR)" --parallel

configure:
	cmake -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DBUILD_TESTING=ON $(CMAKE_ARGS)

test: all
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

rust-test:
	cargo test
	cargo test --features force-scalar

bench:
	./bench/run.sh

install: all
	cmake --install "$(BUILD_DIR)"

clean:
	cmake -E remove_directory "$(BUILD_DIR)"
