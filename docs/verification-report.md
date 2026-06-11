# Verification Report — norxs SOA Gateway v1.0.1

**norxs Technology LLC** · ISO 26262-6 / ASPICE SWE.4–SWE.6 oriented evidence summary.

| Item | Value |
|---|---|
| Report date | 2026-06-11 |
| Source under test | `include/` (9 headers) + `src/` (8 translation units) |
| Build configuration | CMake 3.28, Ninja, GCC 13.3, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -fno-exceptions -fno-rtti`, C++14 |
| Test framework | GoogleTest 1.14 |

---

## 1. Unit Test Results

| Suite | Executable | Cases | Passed | Failed |
|---|---|---|---|---|
| Core modules (SoaServiceManager, IamSecurityController, IpcBridge, HseAdapter, RateLimiter, DdsQosPolicy, DeadSubscriberMonitor, Result types) | `soa_gateway_tests` | 64 | **64** | 0 |
| SafetyArbitrator (ASIL-D state machine, degradation matrix, physical envelope, E-stop latch, MC/DC condition sets) | `safety_arbitrator_tests` | 54 | **54** | 0 |
| **Total** | | **118** | **118** | **0** |

GoogleTest XML reports are produced by CI (`test_results_core.xml`,
`test_results_arbitrator.xml`) and uploaded as workflow artifacts on every push.

## 2. Structural Coverage (gcovr, GCC `--coverage`, host build)

| Metric | Result |
|---|---|
| Line coverage | **80.0 %** (854 / 1067) |
| Function coverage | **86.7 %** (91 / 105) |
| Branch coverage | **70.8 %** (342 / 483) |

Uncovered code is dominated by hardware-bound paths (`HseAdapter` MU register
sequences) and defensive branches unreachable on host. Target-based testing on
the S32G EVB closes these in production engagements; host-side gaps are
tracked in the roadmap (`CHANGELOG.md` Unreleased).

## 3. Static Analysis

| Tool | Configuration | Findings |
|---|---|---|
| cppcheck | `--enable=warning,performance,portability --std=c++14` | **0** errors / warnings / performance / portability findings |
| GCC | `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` | **0** warnings (production + tests) |

## 4. AUTOSAR Compliance Pattern Scan (comment-stripped source)

| Forbidden pattern | Occurrences |
|---|---|
| `try` / `catch` / `throw` | 0 |
| `std::mutex` / `lock_guard` / `unique_lock` | 0 |
| heap allocation (`new`, `malloc`, `calloc`) | 0 |
| dynamic containers (`std::vector`, `std::map`, …) | 0 |
| owning smart pointers | 0 |

## 5. Documentation & Interface Hygiene

| Check | Result |
|---|---|
| Doxygen headers (`@file @brief @author @copyright @standards`) | 17 / 17 production files + 2 / 2 test files |
| Include guards (`NORXS_SOA_*`) | 9 / 9 headers |
| `static_assert` on IPC ABI structs | 6 |
| `noexcept` consistency header ↔ definition | 100 % (verified by `-Wpedantic`) |

## 6. Reproduction

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON
cmake --build build --parallel
./build/soa_gateway_tests && ./build/safety_arbitrator_tests
gcovr --root . --filter 'src/' --filter 'include/' \
      --exclude-unreachable-branches --print-summary build
```

---
*© 2026 norxs Technology LLC.*
