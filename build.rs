//! Builds the bundled C implementation for the Cargo target.

use std::env;

#[derive(Clone, Copy, Eq, PartialEq)]
enum Backend {
    Scalar,
    Avx2,
    Avx512,
}

const AVX2_FEATURES: &[&str] = &["avx2"];

const AVX512_FEATURES: &[&str] = &[
    "avx2",
    "bmi1",
    "movbe",
    "avx512f",
    "avx512dq",
    "avx512bw",
    "avx512vl",
    "avx512ifma",
    "avx512vbmi",
];

fn target_has_all(target_features: &str, required: &[&str]) -> bool {
    required.iter().all(|required| {
        target_features
            .split(',')
            .any(|feature| feature == *required)
    })
}

fn native_build() -> cc::Build {
    let mut build = cc::Build::new();
    build.include("include").include("c").std("c11");
    build
}

fn hide_symbols(build: &mut cc::Build, compiler: &cc::Tool) {
    if compiler.is_like_gnu() || compiler.is_like_clang() {
        build.flag_if_supported("-fvisibility=hidden");
    }
}

fn compile_base(target: &str, compiler: &cc::Tool) {
    let mut build = native_build();
    build
        .file("c/braid58.c")
        .define("BRAID58_BUILDING_LIBRARY", None)
        .define("BRAID58_COMPILED_TARGET", target);
    hide_symbols(&mut build, compiler);
    build.compile("braid58");
}

fn compile_avx2(target: &str, target_features: &str, compiler: &cc::Tool) {
    let mut build = native_build();
    build
        .file("c/encode_avx2.c")
        .file("c/decode_avx2.c")
        .file("c/encode_avx2_64.c")
        .file("c/decode_avx2_64.c")
        .define("BRAID58_COMPILED_TARGET", target)
        .flag("-mavx2")
        .flag("-mtune=haswell")
        .flag("-mno-avx512f");
    for (feature, flag) in [
        ("bmi1", "-mbmi"),
        ("bmi2", "-mbmi2"),
        ("lzcnt", "-mlzcnt"),
        ("movbe", "-mmovbe"),
        ("popcnt", "-mpopcnt"),
    ] {
        if target_has_all(target_features, &[feature]) {
            build.flag(flag);
        }
    }
    hide_symbols(&mut build, compiler);
    build.compile("braid58_avx2");
}

fn compile_avx512(target: &str, compiler: &cc::Tool) {
    let mut build = native_build();
    build
        .file("c/encode_avx512.c")
        .file("c/decode_avx512.c")
        .file("c/encode_avx512_64.c")
        .file("c/decode_avx512_64.c")
        .define("BRAID58_COMPILED_TARGET", target)
        .flag("-mavx2")
        .flag("-mbmi")
        .flag("-mmovbe")
        .flag("-mavx512f")
        .flag("-mavx512dq")
        .flag("-mavx512bw")
        .flag("-mavx512vl")
        .flag("-mavx512ifma")
        .flag("-mavx512vbmi");
    hide_symbols(&mut build, compiler);
    build.compile("braid58_avx512");
}

fn main() {
    for path in [
        "c/braid58.c",
        "c/braid58_internal.h",
        "c/encode_avx2.c",
        "c/decode_avx2.c",
        "c/encode_avx2_64.c",
        "c/decode_avx2_64.c",
        "c/encode_avx512.c",
        "c/decode_avx512.c",
        "c/encode_avx512_64.c",
        "c/decode_avx512_64.c",
        "include/braid58.h",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }

    let compiler = native_build().get_compiler();
    let simd_supported = env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("x86_64")
        && env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc")
        && (compiler.is_like_gnu() || compiler.is_like_clang());
    let force_avx512 = env::var_os("CARGO_FEATURE_AVX512").is_some();
    let force_avx2 = env::var_os("CARGO_FEATURE_AVX2").is_some();
    let target_features = env::var("CARGO_CFG_TARGET_FEATURE").unwrap_or_default();

    assert!(
        !(force_avx2 || force_avx512) || simd_supported,
        "the avx2 and avx512 features require an x86-64 GNU- or Clang-like C compiler"
    );

    let backend = if force_avx512
        || (simd_supported && target_has_all(&target_features, AVX512_FEATURES))
    {
        Backend::Avx512
    } else if force_avx2 || (simd_supported && target_has_all(&target_features, AVX2_FEATURES)) {
        Backend::Avx2
    } else {
        Backend::Scalar
    };
    let target = match backend {
        Backend::Scalar => "0",
        Backend::Avx2 => "1",
        Backend::Avx512 => "2",
    };
    compile_base(target, &compiler);

    if backend == Backend::Avx2 {
        compile_avx2(target, &target_features, &compiler);
    }

    if backend == Backend::Avx512 {
        compile_avx512(target, &compiler);
    }
}
