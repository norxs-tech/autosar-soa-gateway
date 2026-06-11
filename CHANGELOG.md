# Changelog

All notable changes to the **norxs SOA Gateway** reference implementation are
documented here. This project follows [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Planned
- libFuzzer harness on the SOME/IP deserialization path (CI job)
- Signed release artifacts with SLSA provenance attestation
- Target-based (S32G EVB) coverage to close host-unreachable HSE branches

---

## [1.0.1] — 2026-06-11

### Added — Compliance & Supply Chain
- `SECURITY.md`: Coordinated Vulnerability Disclosure policy
  (OpenChain ISO/IEC 18974:2023 §3.2; ISO/IEC 29147 / 30111 aligned)
- `NOTICE.md`: third-party attribution — production library confirmed to have
  **zero third-party runtime dependencies** (OpenChain ISO/IEC 5230 §3.3.2)
- `sbom/norxs-soa-gateway.spdx.json`: SPDX 2.3 SBOM with per-file SHA-1
  checksums for all 18 production/build files
- `docs/compliance/`: requirement-by-requirement mappings for
  OpenChain ISO/IEC 5230:2020, OpenChain ISO/IEC 18974:2023, and NIST CSF 2.0
- `docs/verification-report.md`: executed evidence summary —
  **118/118 unit tests passed**, line/function/branch coverage
  **80.0 % / 86.7 % / 70.8 %**, cppcheck and GCC strict-flag builds clean
- CI Job 5 `supply-chain`: verifies presence of all compliance artifacts,
  validates the SBOM as well-formed SPDX, fails on SBOM ↔ source-tree drift
  (file list + SHA-1), and scans production code for foreign copyright notices

### Fixed
- `noexcept` specifiers added to 27 declarations in `SafetyArbitrator.hpp` and
  `DeadSubscriberMonitor.hpp` whose definitions were already `noexcept`,
  eliminating all `-Wpedantic` exception-specifier mismatch warnings
  (header ↔ implementation interface contract now consistent — AUTOSAR A15-4-4)
- `README.md`: corrected test-case counts (core 64, SafetyArbitrator 54),
  corrected CI badge repository path, documented real coverage figures

---

## [1.0.0] — 2026-01-01

### Initial public release — norxs Technology LLC

This is the first public release of the SOA Gateway for Autonomous
Safety-Supervisor, a production-grade reference implementation targeting the
NXP S32G SoC running QNX on the Cortex-A53 cluster with an AUTOSAR R25-11
Safety Supervisor on the Cortex-M7 cluster.

### Added — Core Modules

#### `SoaServiceManager` (SOME/IP Service Lifecycle)
- Static service registry (`std::array<ServiceDescriptor, 32>`)
- Lock-free SPSC event ring buffer (64 slots, `std::atomic` head/tail)
- SOME/IP SD lifecycle: `OfferService()` / `StopService()` with full state machine
  (`Down → Requested → Available → Stopping → Down`)
- Fan-out subscriber dispatch via plain function pointer table (no `std::function`)
- `Result<T>` / `VoidResult` zero-exception error propagation throughout

#### `NetworkAdapter` (Abstract SOME/IP & DDS Interface)
- Pure abstract base class (`= 0` on all virtual methods)
- `WireFrame` fixed-size buffer (1472 bytes, UDP MTU-safe)
- `SomeIpHeader` struct with big-endian ↔ host byte order helpers (`Be32ToHost`,
  `HostToBe32`) implemented without POSIX `ntohl` for cross-platform portability
- Message type constants: `kSomeIpMsgRequest`, `kSomeIpMsgNotification`, etc.

#### `IamSecurityController` (RBAC Firewall — UN R155)
- Static policy table (`std::array<PolicyEntry, 64>`)
- Wildcard method ID support (`0xFFFF` = all methods)
- `AccessAction` bitmask: `kRead | kWrite | kExecute | kAdmin`
- Default-deny fail-secure policy (no matching entry → `kUnauthorized`)
- Lock-free SPSC audit log ring buffer (128 entries) with POSIX monotonic timestamps
- `DrainAuditLog()` for UN R155 §7.3.2 traceability

#### `IpcBridge` (Cross-Core Shared SRAM + AUTOSAR E2E Profile 5)
- `IpcRingBuffer` struct placed at linker-script address in shared SRAM
- SPSC ring buffer (32 slots, power-of-2 mask arithmetic)
- `E2eProfile5Header`: CRC-16/ARC (table-driven, 256-entry `constexpr` lookup)
  + sequence counter [0..255] + dataId + length
- `std::atomic_thread_fence(seq_cst)` before head-pointer advance (≡ `DMB SY`
  on ARM Cortex-A53 interconnect to M7 TCM)
- `VerifyE2e()` for M7-side or unit-test CRC and counter verification
- `static_assert` on `IpcSlot` and `E2eProfile5Header` layout

### Added — Security Modules

#### `HseAdapter` (NXP S32G Hardware Security Engine)
- MU (Message Unit) register-level driver: `WriteMmioReg` / `ReadMmioReg`
  via `volatile` pointer (no OS HAL dependency)
- `GenerateMac()`: AES-128-CMAC generation via HSE `kMacGenerate` service
- `VerifyMac()`: AES-128-CMAC verification via HSE `kMacVerify` service
- `VerifyCertificate()`: X.509 DER chain verification against provisioned
  root CA (HSE `kCertVerify` service, RSA/ECDSA offload)
- `GenerateRandom()`: TRNG random bytes via HSE `kRngGenerate` (max 64 bytes)
- Synchronous polling with `kHsePollTimeoutUs = 5000` iteration bound
- `static_assert(sizeof(std::uintptr_t) == 4U)` guards against silent pointer
  truncation on 64-bit compilation targets

#### `RateLimiter` (Token Bucket Anti-DDoS — UN R155)
- Per-source Token Bucket: capacity 20 tokens, refill 5 tokens/ms
- Lock-free `Admit()` via CAS on `std::atomic<int32_t>` token counter
- Fail-secure table-full policy: unknown sources denied when 32-entry table
  is exhausted (prevents table-exhaustion spoofing attacks)
- `EvictIdle()`: stale source eviction after `kSourceEvictAfterMs = 5000ms`
- Per-source diagnostic counters: `passCount`, `dropCount`

### Added — Middleware Modules

#### `DdsQosPolicy` (DDS Quality of Service Enforcement)
- `DdsQosProfile` per topic: `ReliabilityKind`, `DurabilityKind`, `HistoryKind`,
  `deadlineMs`, `latencyBudgetMs`, `historyDepth`
- `ValidateSample()`: payload length bounds check + deadline timestamp update
- `CheckDeadlines()`: periodic scan returning first violating topic ID
- `GetViolationCount()`: cumulative deadline miss counter per topic

#### `DeadSubscriberMonitor` (Heartbeat TTL & Connection Pool)
- Heartbeat TTL: `kDeadTimeoutMs = 2000ms`, warning at `kHeartbeatIntervalMs = 500ms`
- Lock-free `RecordHeartbeat()` via `std::atomic<uint64_t>` timestamp
- `ScanAndEvict()`: periodic scan with on-death callback dispatch
- `MonitorStats` snapshot: totalActive / totalAlive / totalWarning / totalDead
- Fail-secure slot release: source ID cleared before `active` flag dropped

### Added — Safety Module

#### `SafetyArbitrator` (ASIL-D Safety State Machine — ISO 26262)
- Full degradation matrix (8 SafeState levels, ISO 26262 Part 3/4 §6.4.6):
  `kFullOperation` → `kRadarCameraFallback` → `kLidarCameraFallback` →
  `kLidarRadarFallback` → `kDeadReckoningMode` → `kReducedDynamics` →
  `kMinimalRiskCondition` → `kEmergencyStop`
- Physical envelope invariants (ROM `constexpr`): `kMaxSteeringAngleDeg = 540°`,
  `kMaxFrictionCoeff = 1.05`, `kMaxLongitudinalDecelMps2 = 9.81`, etc.
- `kSafeStateTransitionMs = 50ms` ISO 26262 deadline with measured latency
  returned as `Err(kTimeout)` on violation
- Emergency stop one-way latch: only `Init()` can clear `kEmergencyStop`
- Mandatory domain bitmask: configurable at `Init()`; fault count ≥ 3 on
  any mandatory domain triggers `kMinimalRiskCondition`
- Heartbeat timeout auto-promotion: domain → `kFailed` after `kSensorTimeoutMs = 100ms`
- `SafeStateCommand` serialised to IPC SRAM (24 bytes, E2E Profile 5 protected)
- ISO 26262 §7.4.2 transition audit log (32-entry circular buffer, newest-first drain)
- Up to 8 registered `SafeStateListener` callbacks per arbitrator instance
- `InjectFault()` API for fault-injection test suite (ASIL-D mandatory)
- `SoaArbitratorEventHandler()` static adaptor for `SoaServiceManager::Subscribe()`
- Static envelope validators: `ValidateSteeringAngle()`, `ValidateFrictionCoeff()`,
  `ValidateDeceleration()`, `ValidateLateralAccel()`

### Added — Build & Quality

#### `CMakeLists.txt`
- `soa_gateway` static library target with all 8 production source files
- `-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Wconversion`
- `BUILD_TESTS=ON`: GoogleTest unit test executables with `gtest_discover_tests`
- `ENABLE_COVERAGE=ON`: `gcov/lcov` instrumentation + `make coverage` target
  (generates `coverage_html/index.html` with branch coverage)
- `make analyze`: `cppcheck` AUTOSAR/MISRA static analysis → XML report
- QNX cross-compilation via `-DCMAKE_TOOLCHAIN_FILE`

#### `cmake/Toolchain-QNX.cmake`
- QNX SDP 8.0 cross-compiler configuration for `aarch64le-unknown-nto-qnx8.0.0`
- Correct `CMAKE_SYSROOT`, `CMAKE_SYSTEM_NAME`, find-root-path settings

#### `cmake/Toolchain-S32G.cmake`
- Bare-metal / AUTOSAR R25-11 toolchain for `aarch64-none-elf` (M7 reference)

#### Unit Tests (`tests/`)
- `test_soa_gateway.cpp`: 60+ GoogleTest cases across all 6 core modules
- `test_safety_arbitrator.cpp`: 52 GoogleTest cases, 15 test groups (T1–T15)
  covering full MC/DC for `SafetyArbitrator`
- All `static_assert` size checks verified at compile time
- Fault injection tests: CRC corruption, ring-full, E2E counter mismatch

### Compliance Summary (v1.0.0)

| Standard | Coverage |
|----------|----------|
| AUTOSAR C++14 (MISRA C++:2008 base) | `-fno-exceptions`, `-fno-rtti`, zero dynamic allocation, `Result<T>` |
| ISO 26262 Part 3/4/6 (ASIL-D) | `SafetyArbitrator` degradation matrix, 50ms transition deadline, E2E Profile 5 |
| UN R155 / ISO 21434 | `IamSecurityController` RBAC, `HseAdapter` SecOC, `RateLimiter` anti-DDoS, audit log |
| AUTOSAR E2E Profile 5 | CRC-16/ARC + sequence counter on every `IpcSlot` |
| POSIX (QNX 8.0) | `clock_gettime(CLOCK_MONOTONIC)` throughout; no OS-specific APIs beyond POSIX |

---

*For commercial licensing, safety evidence packages, and ASIL-D certification
support, contact norxs Technology LLC at https://norxs.com*
