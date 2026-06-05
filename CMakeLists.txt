cmake_minimum_required(VERSION 3.16)
project(norxs_soa_gateway VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# AUTOSAR C++14 compliance flags
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Compiler flags (GCC/Clang – adapt for QNX qcc as needed)
# ---------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wno-unused-parameter
    -fno-exceptions
    -fno-rtti
    -fstack-usage
)

# ---------------------------------------------------------------------------
# Coverage flags (enabled via -DENABLE_COVERAGE=ON)
# ---------------------------------------------------------------------------
option(ENABLE_COVERAGE "Enable gcov/lcov coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
    add_compile_options(-fprofile-arcs -ftest-coverage -O0 -g)
    add_link_options(-fprofile-arcs -ftest-coverage)
endif()

# ---------------------------------------------------------------------------
# SOA Gateway static library
# ---------------------------------------------------------------------------
add_library(soa_gateway STATIC
    src/SoaServiceManager.cpp
    src/IamSecurityController.cpp
    src/IpcBridge.cpp
    src/HseAdapter.cpp
    src/RateLimiter.cpp
    src/DdsQosPolicy.cpp
    src/DeadSubscriberMonitor.cpp
    src/SafetyArbitrator.cpp
)

target_include_directories(soa_gateway
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(soa_gateway PUBLIC cxx_std_14)

# ---------------------------------------------------------------------------
# Unit test executable (GoogleTest)
# Build with: cmake -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON ..
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build GoogleTest unit test suite" OFF)
if(BUILD_TESTS)
    find_package(GTest REQUIRED)

    add_executable(soa_gateway_tests
        tests/test_soa_gateway.cpp
    )

    target_include_directories(soa_gateway_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(soa_gateway_tests
        PRIVATE
            soa_gateway
            GTest::GTest
            GTest::Main
    )

    # gtest itself requires exceptions/RTTI — override for test binary only
    target_compile_options(soa_gateway_tests
        PRIVATE
            -fexceptions
            -frtti
            -Wno-conversion
    )

    target_compile_features(soa_gateway_tests PRIVATE cxx_std_14)

    include(GoogleTest)
    gtest_discover_tests(soa_gateway_tests
        EXTRA_ARGS --gtest_output=xml:${CMAKE_BINARY_DIR}/test_results.xml
    )

    # ---------------------------------------------------------------------------
    # Coverage report target (lcov + genhtml)
    # Run: make coverage  (after running soa_gateway_tests)
    # ---------------------------------------------------------------------------
    if(ENABLE_COVERAGE)
        find_program(LCOV_EXE lcov)
        find_program(GENHTML_EXE genhtml)
        if(LCOV_EXE AND GENHTML_EXE)
            add_custom_target(coverage
                COMMAND ${LCOV_EXE} --capture --directory .
                        --output-file lcov_raw.info
                        --rc lcov_branch_coverage=1
                COMMAND ${LCOV_EXE} --remove lcov_raw.info
                        '/usr/*' '*/tests/*' '*/gtest/*'
                        --output-file lcov.info
                        --rc lcov_branch_coverage=1
                COMMAND ${GENHTML_EXE} lcov.info
                        --output-directory coverage_html
                        --branch-coverage
                        --title "norxs SOA Gateway MC/DC Coverage"
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating MC/DC HTML coverage -> coverage_html/index.html"
                DEPENDS soa_gateway_tests
            )
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Static analysis target (cppcheck)
# Run: make analyze
# ---------------------------------------------------------------------------
find_program(CPPCHECK_EXE cppcheck)
if(CPPCHECK_EXE)
    add_custom_target(analyze
        COMMAND ${CPPCHECK_EXE}
            --enable=all
            --std=c++14
            --language=c++
            --suppress=missingIncludeSystem
            --error-exitcode=1
            --xml
            --output-file=${CMAKE_BINARY_DIR}/cppcheck_report.xml
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        COMMENT "Running cppcheck AUTOSAR/MISRA static analysis"
    )
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS soa_gateway ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include/norxs_soa)

# Safety Arbitrator test binary
if(BUILD_TESTS)
    add_executable(safety_arbitrator_tests
        tests/test_safety_arbitrator.cpp
    )
    target_include_directories(safety_arbitrator_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(safety_arbitrator_tests
        PRIVATE soa_gateway GTest::GTest GTest::Main
    )
    target_compile_options(safety_arbitrator_tests
        PRIVATE -fexceptions -frtti -Wno-conversion
    )
    target_compile_features(safety_arbitrator_tests PRIVATE cxx_std_14)
    gtest_discover_tests(safety_arbitrator_tests
        EXTRA_ARGS --gtest_output=xml:${CMAKE_BINARY_DIR}/safety_arbitrator_results.xml
    )
endif()
