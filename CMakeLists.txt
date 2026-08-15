cmake_minimum_required(VERSION 3.16)
project(braid58 VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BRAID58_ENABLE_AVX512 "Build the runtime-dispatched AVX-512 backend" ON)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" BRAID58_PROCESSOR)
set(BRAID58_HAVE_AVX512_KERNEL OFF)
if(BRAID58_ENABLE_AVX512
   AND BRAID58_PROCESSOR MATCHES "^(x86_64|amd64)$"
   AND CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  set(BRAID58_HAVE_AVX512_KERNEL ON)
endif()

set(BRAID58_SOURCES c/braid58.c)
if(BRAID58_HAVE_AVX512_KERNEL)
  list(APPEND BRAID58_SOURCES c/encode_avx512.c c/decode_avx512.c)
endif()

add_library(braid58_objects OBJECT ${BRAID58_SOURCES})
set_target_properties(braid58_objects PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
target_compile_features(braid58_objects PRIVATE c_std_11)
target_include_directories(braid58_objects PRIVATE include c)
target_compile_definitions(braid58_objects PRIVATE
  BRAID58_BUILDING_LIBRARY
  BRAID58_HAVE_AVX512_KERNEL=$<BOOL:${BRAID58_HAVE_AVX512_KERNEL}>)

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
    BRAID58_HAVE_AVX512_KERNEL=$<BOOL:${BRAID58_HAVE_AVX512_KERNEL}>)
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
