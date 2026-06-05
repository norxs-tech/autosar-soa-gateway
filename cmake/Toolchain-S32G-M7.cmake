# ==============================================================================
# cmake/Toolchain-S32G-M7.cmake
# Bare-metal / AUTOSAR R25-11 toolchain for NXP S32G Cortex-M7 cluster
#
# Usage:
#   cmake -B build-m7 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-S32G-M7.cmake \
#         -DGCC_ARM_PATH=/path/to/arm-gnu-toolchain-13 \
#         -DCMAKE_BUILD_TYPE=Release
#
# Prerequisites:
#   - ARM GNU Toolchain 13+ (arm-none-eabi) from developer.arm.com
#   - NXP S32DS or equivalent linker script for S32G M7 TCM memory layout
#
# This toolchain targets the C-only AUTOSAR R25-11 Safety Supervisor running
# on the M7 cluster. It is provided as a reference; the full AUTOSAR BSW stack
# is supplied separately under a commercial license agreement.
#
# norxs Technology LLC — (c) 2026 All rights reserved.
# ==============================================================================

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  arm)

# ---------------------------------------------------------------------------
# Toolchain path — override with -DGCC_ARM_PATH
# ---------------------------------------------------------------------------
if(NOT DEFINED GCC_ARM_PATH)
    if(DEFINED ENV{GCC_ARM_PATH})
        set(GCC_ARM_PATH "$ENV{GCC_ARM_PATH}")
    else()
        # Try to find arm-none-eabi-gcc on PATH
        find_program(ARM_GCC arm-none-eabi-gcc)
        if(ARM_GCC)
            get_filename_component(GCC_ARM_PATH "${ARM_GCC}" DIRECTORY)
            get_filename_component(GCC_ARM_PATH "${GCC_ARM_PATH}" DIRECTORY)
        else()
            message(FATAL_ERROR
                "arm-none-eabi-gcc not found. Install the ARM GNU Toolchain and "
                "pass -DGCC_ARM_PATH=/path/to/arm-gnu-toolchain-13")
        endif()
    endif()
endif()

message(STATUS "GCC_ARM_PATH: ${GCC_ARM_PATH}")

set(TOOLCHAIN_PREFIX "${GCC_ARM_PATH}/bin/arm-none-eabi-")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}gcc"   CACHE FILEPATH "ARM C compiler")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++"   CACHE FILEPATH "ARM C++ compiler")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}gcc"   CACHE FILEPATH "ARM assembler")
set(CMAKE_AR           "${TOOLCHAIN_PREFIX}ar"    CACHE FILEPATH "ARM archiver")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_PREFIX}objcopy" CACHE FILEPATH "objcopy")
set(CMAKE_SIZE         "${TOOLCHAIN_PREFIX}size"    CACHE FILEPATH "size")

# ---------------------------------------------------------------------------
# NXP S32G Cortex-M7 CPU flags
# Cortex-M7 @ 400 MHz, FPU (FPv5-D16), TCM, No OS
# ---------------------------------------------------------------------------
set(M7_CPU_FLAGS
    "-mcpu=cortex-m7"
    "-mthumb"
    "-mfpu=fpv5-d16"
    "-mfloat-abi=hard"
)
string(JOIN " " M7_CPU_STR ${M7_CPU_FLAGS})

# ---------------------------------------------------------------------------
# Compiler flags (MISRA C:2012 / AUTOSAR Adaptive C++14 for M7 is C only)
# ---------------------------------------------------------------------------
set(M7_COMMON_FLAGS
    "${M7_CPU_STR}"
    "-fno-exceptions"
    "-fno-rtti"
    "-fno-common"
    "-ffunction-sections"        # Enable dead code elimination
    "-fdata-sections"
    "-ffreestanding"             # Bare-metal: no hosted standard library assumed
    "-Wall"
    "-Wextra"
    "-Wpedantic"
    "-Wno-unused-parameter"
    "-DNXP_S32G_M7"              # Platform identification macro
    "-DAUTOSAR_R25_11"           # AUTOSAR version macro
)
string(JOIN " " M7_FLAGS_STR ${M7_COMMON_FLAGS})

set(CMAKE_C_FLAGS_INIT   "${M7_FLAGS_STR} -std=c11")
set(CMAKE_CXX_FLAGS_INIT "${M7_FLAGS_STR} -std=c++14")

# Release: -O2 for deterministic timing (ISO 26262 Part 6 §8.4.5)
set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "")

# ---------------------------------------------------------------------------
# Linker flags (bare-metal, GC sections, map file generation)
# ---------------------------------------------------------------------------
set(M7_LINKER_FLAGS
    "${M7_CPU_STR}"
    "-nostartfiles"
    "-Wl,--gc-sections"
    "-Wl,--print-memory-usage"
    "-Wl,-Map=\${TARGET_NAME}.map,--cref"
)
string(JOIN " " M7_LINK_STR ${M7_LINKER_FLAGS})

set(CMAKE_EXE_LINKER_FLAGS_INIT "${M7_LINK_STR}")

# ---------------------------------------------------------------------------
# Find root (no sysroot for bare-metal)
# ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
