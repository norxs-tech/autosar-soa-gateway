# ==============================================================================
# cmake/Toolchain-QNX.cmake
# QNX SDP 8.0 Cross-Compilation Toolchain for NXP S32G (Cortex-A53 / aarch64le)
#
# Usage:
#   cmake -B build \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-QNX.cmake \
#         -DQNX_HOST=/path/to/qnx800/host/linux/x86_64 \
#         -DQNX_TARGET=/path/to/qnx800/target/qnx8 \
#         -DCMAKE_BUILD_TYPE=Release
#
# Prerequisites:
#   - QNX SDP 8.0 installed (source qnxsdp-env.sh before invoking cmake)
#   - Environment variable QNX_HOST and QNX_TARGET set by qnxsdp-env.sh
#
# norxs Technology LLC — (c) 2026 All rights reserved.
# ==============================================================================

set(CMAKE_SYSTEM_NAME       QNX)
set(CMAKE_SYSTEM_PROCESSOR  aarch64)
set(CMAKE_SYSTEM_VERSION    8.0.0)

# ---------------------------------------------------------------------------
# QNX SDP paths — override with -DQNX_HOST / -DQNX_TARGET if not in env
# ---------------------------------------------------------------------------
if(NOT DEFINED QNX_HOST)
    if(DEFINED ENV{QNX_HOST})
        set(QNX_HOST "$ENV{QNX_HOST}")
    else()
        message(FATAL_ERROR
            "QNX_HOST not set. Source qnxsdp-env.sh or pass "
            "-DQNX_HOST=/path/to/qnx800/host/linux/x86_64")
    endif()
endif()

if(NOT DEFINED QNX_TARGET)
    if(DEFINED ENV{QNX_TARGET})
        set(QNX_TARGET "$ENV{QNX_TARGET}")
    else()
        message(FATAL_ERROR
            "QNX_TARGET not set. Source qnxsdp-env.sh or pass "
            "-DQNX_TARGET=/path/to/qnx800/target/qnx8")
    endif()
endif()

message(STATUS "QNX_HOST  : ${QNX_HOST}")
message(STATUS "QNX_TARGET: ${QNX_TARGET}")

# ---------------------------------------------------------------------------
# Compiler binaries
# Target triple: aarch64le-unknown-nto-qnx8.0.0
# ---------------------------------------------------------------------------
set(QNX_TRIPLE "aarch64le-unknown-nto-qnx8.0.0")

set(CMAKE_C_COMPILER
    "${QNX_HOST}/usr/bin/qcc"
    CACHE FILEPATH "QNX C compiler")
set(CMAKE_CXX_COMPILER
    "${QNX_HOST}/usr/bin/q++"
    CACHE FILEPATH "QNX C++ compiler")
set(CMAKE_ASM_COMPILER
    "${QNX_HOST}/usr/bin/qcc"
    CACHE FILEPATH "QNX assembler")

# qcc/q++ target flags
set(CMAKE_C_COMPILER_TARGET   "${QNX_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${QNX_TRIPLE}")

# ---------------------------------------------------------------------------
# Sysroot
# ---------------------------------------------------------------------------
set(CMAKE_SYSROOT "${QNX_TARGET}")

set(CMAKE_FIND_ROOT_PATH
    "${QNX_TARGET}/aarch64le"
    "${QNX_HOST}/usr"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---------------------------------------------------------------------------
# QNX-specific compiler flags (AUTOSAR C++14, no exceptions, no RTTI)
# ---------------------------------------------------------------------------
set(QNX_COMMON_FLAGS
    "-D_QNX_SOURCE"          # Expose QNX-specific POSIX extensions
    "-D__EXT_POSIX1_200112"  # POSIX.1-2001 feature test macro
    "-Wall"
    "-Wextra"
    "-Wpedantic"
    "-fno-exceptions"        # AUTOSAR: exceptions forbidden
    "-fno-rtti"              # AUTOSAR: RTTI forbidden
    "-fstack-protector-strong"
)

string(JOIN " " QNX_FLAGS_STR ${QNX_COMMON_FLAGS})

set(CMAKE_C_FLAGS_INIT   "${QNX_FLAGS_STR}")
set(CMAKE_CXX_FLAGS_INIT "${QNX_FLAGS_STR} -std=c++14")

# ---------------------------------------------------------------------------
# NXP S32G Cortex-A53 CPU tuning
# (4x Cortex-A53 @ up to 1.0 GHz, ARMv8-A)
# ---------------------------------------------------------------------------
set(S32G_CPU_FLAGS "-mcpu=cortex-a53 -march=armv8-a")
set(CMAKE_C_FLAGS_INIT   "${CMAKE_C_FLAGS_INIT} ${S32G_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${S32G_CPU_FLAGS}")

# ---------------------------------------------------------------------------
# Release optimisation (ISO 26262 recommends -O2 for deterministic codegen)
# ---------------------------------------------------------------------------
set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "")

# ---------------------------------------------------------------------------
# Linker
# ---------------------------------------------------------------------------
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-lang-c++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-lang-c++")

# ---------------------------------------------------------------------------
# Skip compiler sanity check (cross-compiling — executables won't run on host)
# ---------------------------------------------------------------------------
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
