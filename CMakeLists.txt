cmake_minimum_required(VERSION 3.16)
project(braid58 VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(BRAID58_TARGET "scalar" CACHE STRING
  "Compile target: scalar, avx2, avx512, or native")
set_property(CACHE BRAID58_TARGET PROPERTY STRINGS scalar avx2 avx512 native)
string(TOLOWER "${BRAID58_TARGET}" BRAID58_REQUESTED_TARGET)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" BRAID58_PROCESSOR)
set(BRAID58_X86_64 OFF)
if(BRAID58_PROCESSOR MATCHES "^(x86_64|amd64)$")
  set(BRAID58_X86_64 ON)
endif()
set(BRAID58_SIMD_COMPILER OFF)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  set(BRAID58_SIMD_COMPILER ON)
endif()

if(BRAID58_REQUESTED_TARGET STREQUAL "native")
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
      "BRAID58_TARGET=native cannot be used while cross-compiling; choose an explicit target")
  endif()
  set(BRAID58_RESOLVED_TARGET scalar)
  if(BRAID58_X86_64 AND BRAID58_SIMD_COMPILER)
    include(CheckCSourceCompiles)
    set(BRAID58_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -march=native")
    check_c_source_compiles([[
#ifndef __AVX2__
#error AVX2 is unavailable
#endif
#ifndef __BMI__
#error BMI1 is unavailable
#endif
#ifndef __MOVBE__
#error MOVBE is unavailable
#endif
int main(void) { return 0; }
    ]] BRAID58_NATIVE_HAS_AVX2)
    check_c_source_compiles([[
#ifndef __AVX2__
#error AVX2 is unavailable
#endif
#ifndef __AVX512F__
#error AVX-512 F is unavailable
#endif
#ifndef __AVX512DQ__
#error AVX-512 DQ is unavailable
#endif
#ifndef __AVX512BW__
#error AVX-512 BW is unavailable
#endif
#ifndef __AVX512VL__
#error AVX-512 VL is unavailable
#endif
#ifndef __AVX512IFMA__
#error AVX-512 IFMA is unavailable
#endif
#ifndef __AVX512VBMI__
#error AVX-512 VBMI is unavailable
#endif
#ifndef __BMI__
#error BMI1 is unavailable
#endif
#ifndef __MOVBE__
#error MOVBE is unavailable
#endif
int main(void) { return 0; }
    ]] BRAID58_NATIVE_HAS_AVX512)
    set(CMAKE_REQUIRED_FLAGS "${BRAID58_SAVED_REQUIRED_FLAGS}")
    if(BRAID58_NATIVE_HAS_AVX512)
      set(BRAID58_RESOLVED_TARGET avx512)
    elseif(BRAID58_NATIVE_HAS_AVX2)
      set(BRAID58_RESOLVED_TARGET avx2)
    endif()
  endif()
elseif(BRAID58_REQUESTED_TARGET MATCHES "^(scalar|avx2|avx512)$")
  set(BRAID58_RESOLVED_TARGET "${BRAID58_REQUESTED_TARGET}")
else()
  message(FATAL_ERROR
    "Unknown BRAID58_TARGET=${BRAID58_TARGET}; expected scalar, avx2, avx512, or native")
endif()

if(NOT BRAID58_RESOLVED_TARGET STREQUAL "scalar" AND
   (NOT BRAID58_X86_64 OR NOT BRAID58_SIMD_COMPILER))
  message(FATAL_ERROR
    "BRAID58_TARGET=${BRAID58_RESOLVED_TARGET} requires x86-64 and GCC or Clang")
endif()

set(BRAID58_HAVE_AVX2_KERNEL OFF)
set(BRAID58_HAVE_AVX512_KERNEL OFF)
set(BRAID58_TARGET_VALUE 0)
if(BRAID58_RESOLVED_TARGET STREQUAL "avx2")
  set(BRAID58_HAVE_AVX2_KERNEL ON)
  set(BRAID58_TARGET_VALUE 1)
elseif(BRAID58_RESOLVED_TARGET STREQUAL "avx512")
  set(BRAID58_HAVE_AVX512_KERNEL ON)
  set(BRAID58_TARGET_VALUE 2)
endif()
message(STATUS "Braid58 compile target: ${BRAID58_RESOLVED_TARGET}")

set(BRAID58_SOURCES c/braid58.c)
if(BRAID58_HAVE_AVX2_KERNEL)
  list(APPEND BRAID58_SOURCES
    c/encode_avx2.c
    c/decode_avx2.c
    c/encode_avx2_64.c
    c/decode_avx2_64.c)
  set_property(SOURCE
    c/encode_avx2.c
    c/decode_avx2.c
    c/encode_avx2_64.c
    c/decode_avx2_64.c
    APPEND PROPERTY COMPILE_OPTIONS -march=haswell -mno-avx512f)
endif()
if(BRAID58_HAVE_AVX512_KERNEL)
  list(APPEND BRAID58_SOURCES
    c/encode_avx512.c
    c/decode_avx512.c
    c/encode_avx512_64.c
    c/decode_avx512_64.c)
  set_property(SOURCE
    c/encode_avx512.c
    c/decode_avx512.c
    c/encode_avx512_64.c
    c/decode_avx512_64.c
    APPEND PROPERTY COMPILE_OPTIONS
      -mavx2
      -mbmi
      -mmovbe
      -mavx512f
      -mavx512dq
      -mavx512bw
      -mavx512vl
      -mavx512ifma
      -mavx512vbmi)
endif()

add_library(braid58_objects OBJECT ${BRAID58_SOURCES})
set_target_properties(braid58_objects PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
target_compile_features(braid58_objects PRIVATE c_std_11)
target_include_directories(braid58_objects PRIVATE include c)
target_compile_definitions(braid58_objects PRIVATE
  BRAID58_BUILDING_LIBRARY
  BRAID58_COMPILED_TARGET=${BRAID58_TARGET_VALUE})

if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  target_compile_options(braid58_objects PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(braid58 $<TARGET_OBJECTS:braid58_objects>)
add_library(braid58::braid58 ALIAS braid58)
set_target_properties(braid58 PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  PUBLIC_HEADER include/braid58.h)
target_include_directories(braid58 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(BUILD_SHARED_LIBS AND WIN32)
  target_compile_definitions(braid58_objects PRIVATE BRAID58_SHARED)
  target_compile_definitions(braid58 INTERFACE BRAID58_SHARED)
endif()

if(BUILD_TESTING)
  add_executable(braid58_test
    tests/test_c.c
    $<TARGET_OBJECTS:braid58_objects>)
  target_compile_features(braid58_test PRIVATE c_std_11)
  target_include_directories(braid58_test PRIVATE include c)
  target_compile_definitions(braid58_test PRIVATE
    BRAID58_COMPILED_TARGET=${BRAID58_TARGET_VALUE})
  if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(braid58_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME braid58.c COMMAND braid58_test)
endif()

configure_package_config_file(
  cmake/braid58Config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/braid58Config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/braid58")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/braid58ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
configure_file(cmake/braid58.pc.in braid58.pc @ONLY)

install(TARGETS braid58 EXPORT braid58Targets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT braid58Targets
  FILE braid58Targets.cmake
  NAMESPACE braid58::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/braid58")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/braid58Config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/braid58ConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/braid58")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/braid58.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
