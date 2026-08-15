//! Builds the bundled C implementation for the Cargo target.

use std::env;

fn main() {
    for path in [
        "c/braid58.c",
        "c/braid58_internal.h",
        "c/encode_avx2.c",
        "c/decode_avx2.c",
        "c/encode_avx512.c",
        "c/decode_avx512.c",
        "include/braid58.h",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }

    let mut build = cc::Build::new();
    build
        .file("c/braid58.c")
        .include("include")
        .include("c")
        .std("c11")
        .define("BRAID58_BUILDING_LIBRARY", None);

    let compiler = build.get_compiler();
    let native_x86_64 = env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("x86_64")
        && env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc")
        && (compiler.is_like_gnu() || compiler.is_like_clang())
        && env::var_os("CARGO_FEATURE_FORCE_SCALAR").is_none();

    if native_x86_64 {
        build
            .file("c/encode_avx2.c")
            .file("c/decode_avx2.c")
            .file("c/encode_avx512.c")
            .file("c/decode_avx512.c")
            .define("BRAID58_HAVE_AVX2_KERNEL", "1")
            .define("BRAID58_HAVE_AVX512_KERNEL", "1");
    } else {
        build
            .define("BRAID58_HAVE_AVX2_KERNEL", "0")
            .define("BRAID58_HAVE_AVX512_KERNEL", "0");
    }

    if compiler.is_like_gnu() || compiler.is_like_clang() {
        build.flag_if_supported("-fvisibility=hidden");
    }
    build.compile("braid58");
}
