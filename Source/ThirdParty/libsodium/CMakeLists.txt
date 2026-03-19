# Copyright (c) 2008-2022 the Urho3D project
# License: MIT
# libsodium 1.0.21 — ISC License (see licenses/libsodium/LICENSE)

# Define target name
set (TARGET_NAME libsodium)

# Platform configuration — replaces autotools configure step
add_definitions (
    -DCONFIGURED=1
    -DSODIUM_STATIC=1
    -DHAVE_INLINE_ASM=1
    -DHAVE_C_VARARRAYS=1
)

# Endianness
include (TestBigEndian)
test_big_endian (IS_BIG_ENDIAN)
if (NOT IS_BIG_ENDIAN)
    add_definitions (-DNATIVE_LITTLE_ENDIAN=1)
endif ()

# x86_64 specifics — ASM enabled (sandy2x curve25519, xmm6 salsa20)
if (CMAKE_SIZEOF_VOID_P EQUAL 8 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64"))
    enable_language (ASM)
    add_definitions (
        -DHAVE_AMD64_ASM=1
        -DHAVE_AVX_ASM=1
        -DHAVE_CPUID=1
        -DHAVE_TI_MODE=1
        -DHAVE_MMINTRIN_H=1
        -DHAVE_EMMINTRIN_H=1
        -DHAVE_PMMINTRIN_H=1
        -DHAVE_TMMINTRIN_H=1
        -DHAVE_SMMINTRIN_H=1
        -DHAVE_AVXINTRIN_H=1
        -DHAVE_AVX2INTRIN_H=1
    )
endif ()

# POSIX features
if (NOT WIN32)
    add_definitions (
        -DHAVE_POSIX_MEMALIGN=1
        -DHAVE_MMAP=1
        -DHAVE_MLOCK=1
        -DHAVE_MPROTECT=1
        -DHAVE_MADVISE=1
        -DHAVE_NANOSLEEP=1
        -DHAVE_ARC4RANDOM=0
        -DHAVE_GETRANDOM=1
        -D_GNU_SOURCE=1
    )
endif ()

# Suppress warnings — third-party code
if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
endif ()

# Define source files — include .S assembly for x86_64 ASM implementations
define_source_files (GLOB_CPP_PATTERNS *.c *.S RECURSE)

# Define include paths — source files use bare includes like "core.h" which live in include/sodium/
set (INCLUDE_DIRS include include/sodium)

# Setup target
setup_library ()

# Install headers for building the Urho3D library
install_header_files (DIRECTORY include/ DESTINATION ${DEST_INCLUDE_DIR}/ThirdParty/libsodium FILES_MATCHING PATTERN *.h BUILD_TREE_ONLY)
