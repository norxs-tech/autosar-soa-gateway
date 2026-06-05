# norxs SOA Gateway
### SOME/IP & DDS Integration for NXP S32G — ASIL-D Reference Implementation

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

[![CI](https://github.com/norxs-lab/soa-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/norxs-lab/soa-gateway/actions)
[![License](https://img.shields.io/badge/license-norxs%20RI%20v1.0-blue)](LICENSE)
[![Standard](https://img.shields.io/badge/standard-AUTOSAR%20C%2B%2B14-green)]()
[![Safety](https://img.shields.io/badge/safety-ISO%2026262%20ASIL--D-red)]()
[![Security](https://img.shields.io/badge/security-UN%20R155%20%7C%20ISO%2021434-orange)]()

---

## What This Is

A production-grade C++ reference implementation of a **Safety-Oriented Architecture
(SOA) Gateway** running on the NXP S32G SoC. It acts as the secure middleware bridge
between high-performance AI perception domains (e.g., NVIDIA Orin) and an ASIL-D
Cortex-M7 Safety Supervisor, enforcing functional safety, cybersecurity, and
communication quality requirements from a single coherent codebase.

**This is the software we build for our clients — shown here as a reference.**

---

## System Architecture

```
┌─────────────────────────────────────────────────────┐
│        AI Domain  (e.g., NVIDIA Orin)               │
│        ADAS Perception · Planning · ROS2/DDS        │
└────────────────────────┬────────────────────────────┘
                         │  Automotive Ethernet (1000BASE-T1)
                         │  SOME/IP over UDP  |  DDS over RTPS
                         ▼
┌─────────────────────────────────────────────────────┐
│   NXP S32G SoC — Cortex-A53 Cluster (QNX 8.0)      │
│                                                     │
│   ┌───────────────┐     ┌───────────────────────┐   │
│   │NetworkAdapter │────▶│  RateLimiter          │   │
│   │(SOME/IP·DDS)  │     │  (Token Bucket,       │   │
│   └───────────────┘     │   20 burst, 5tok/ms)  │   │
│                         └──────────┬────────────┘   │
│   ┌───────────────┐                │                │
│   │  HseAdapter   │     ┌──────────▼────────────┐   │
│   │  (AES-CMAC,   │────▶│IamSecurityController  │   │
│   │   X.509,TRNG) │     │(RBAC, UN R155 audit)  │   │
│   └───────────────┘     └──────────┬────────────┘   │
│                                    │ authorised      │
│   ┌───────────────┐     ┌──────────▼────────────┐   │
│   │DdsQosEnforcer │     │  SoaServiceManager    │   │
│   │(Reliability,  │────▶│  (SD lifecycle,       │   │
│   │ Deadline,     │     │   SPSC Pub/Sub,       │   │
│   │ History)      │     │   lock-free dispatch) │   │
│   └───────────────┘     └──────────┬────────────┘   │
│                                    │ SoaEvent        │
│   ┌───────────────┐     ┌──────────▼────────────┐   │
│   │DeadSubscriber │     │  SafetyArbitrator     │   │
│   │Monitor        │     │  (ASIL-D state mach., │   │
│   │(heartbeat TTL,│     │   degradation matrix, │   │
│   │ 2000ms evict) │     │   50ms ISO 26262)     │   │
│   └───────────────┘     └──────────┬────────────┘   │
│                                    │ SafeStateCmd    │
│                         ┌──────────▼────────────┐   │
│                         │     IpcBridge         │   │
│                         │  (SPSC ring, E2E P5,  │   │
│                         │   seq_cst barriers)   │   │
│                         └──────────┬────────────┘   │
└────────────────────────────────────┼────────────────┘
                                     │
              Shared SRAM  ──────────┘  (Cache-Inhibited, MPU-protected)
              IpcRingBuffer: 32 slots × 148 bytes
              E2E Profile 5: CRC-16/ARC + Sequence Counter
                                     │
┌────────────────────────────────────▼────────────────┐
│   NXP S32G SoC — Cortex-M7 Cluster (AUTOSAR R25-11) │
│   ASIL-D Safety Supervisor                          │
│   WdgM · ComM · Dem · FiM                           │
└─────────────────────────────────────────────────────┘
```

---

## Module Inventory

| Module | File(s) | Purpose | Standard |
|--------|---------|---------|----------|
| **SoaServiceManager** | `SoaServiceManager.hpp/cpp` | SOME/IP SD lifecycle, lock-free SPSC Pub/Sub event dispatch | AUTOSAR AP SOME/IP |
| **NetworkAdapter** | `NetworkAdapter.hpp` | Pure abstract interface for SOME/IP & DDS protocol adapters | SOME/IP TR |
| **IamSecurityController** | `IamSecurityController.hpp/cpp` | RBAC firewall, wildcard policy, UN R155 audit log (128 entries) | UN R155 §7.3.2 |
| **IpcBridge** | `IpcBridge.hpp/cpp` | Cross-core SPSC ring (32 slots), AUTOSAR E2E Profile 5 CRC-16/ARC + seq counter | AUTOSAR SWS_E2ELibrary §7.6 |
| **HseAdapter** | `HseAdapter.hpp/cpp` | NXP S32G HSE MU driver: AES-128-CMAC, X.509 cert verify, TRNG | NXP HSE FW Ref §4.2 |
| **RateLimiter** | `RateLimiter.hpp/cpp` | Token Bucket per-source anti-DDoS, lock-free CAS, fail-secure table-full | UN R155 §7.3.3 |
| **DdsQosPolicy** | `DdsQosPolicy.hpp/cpp` | DDS QoS profiles: Reliability, Deadline, Latency Budget, History | OMG DDS 1.4 |
| **DeadSubscriberMonitor** | `DeadSubscriberMonitor.hpp/cpp` | Heartbeat TTL 2000ms, `ScanAndEvict()`, on-death callback | AUTOSAR AP |
| **SafetyArbitrator** | `SafetyArbitrator.hpp/cpp` | ASIL-D degradation matrix (8 states), 50ms transition deadline, physical envelope ROM invariants, E-stop latch | ISO 26262 Part 3/4/6 |

---

## Safety Degradation Matrix (ISO 26262 Part 4 §6.4.6)

| Fault Condition | Safe State | Speed Cap | Transition |
|----------------|-----------|-----------|-----------|
| All nominal | `kFullOperation` | 130 kph | — |
| LiDAR failed | `kRadarCameraFallback` | 100 kph | ≤ 50 ms |
| Radar failed | `kLidarCameraFallback` | 100 kph | ≤ 50 ms |
| Camera failed | `kLidarRadarFallback` | 30 kph | ≤ 50 ms |
| GNSS failed | `kDeadReckoningMode` | 30 kph | ≤ 50 ms |
| IMU failed | `kReducedDynamics` | 30 kph | ≤ 50 ms |
| **LiDAR + Radar** | **`kMinimalRiskCondition`** | **20 kph → pull over 10s** | **≤ 50 ms** |
| ASIL-D / E2E violation | **`kEmergencyStop`** ⚠ | **0 kph (latch)** | **Immediate** |

---

## Compliance Summary

### Phase 1 — System & Hardware Isolation ✅
- `IpcBridge`: `memset` + dual `seq_cst` fence before `magic` write = cache coherency guarantee
- `IpcRingBuffer*` injected via linker-script address; A53 holds write-only access to IPC SRAM
- Zero `std::mutex` on any data path; all cross-thread state via `std::atomic<T>`

### Phase 2 — ISO 26262 ASIL-D ✅
- `SafetyArbitrator`: 8-level degradation matrix, 50ms bounded transition, ISO 26262 §7.4.2 audit trail
- `E2eProfile5Header`: CRC-16/ARC (256-entry `constexpr` table) + sequence counter [0..255]
- Physical invariants (`kMaxSteeringAngleDeg`, `kMaxFrictionCoeff`, etc.) in `.rodata`

### Phase 3 — UN R155 / ISO 21434 ✅
- `IamSecurityController`: principal × service × method RBAC, default-deny, 128-entry audit log
- `HseAdapter`: AES-128-CMAC SecOC, RSA/ECDSA X.509 chain, TRNG — all via NXP HSE MU
- `RateLimiter`: Token Bucket, lock-free CAS, fail-secure on table exhaustion (32 sources max)

### Phase 4 — Middleware & SOA ✅
- `SoaServiceManager`: SD offer/stop with `Down→Available→Stopping→Down` state machine
- Zero-copy pipeline: stack-allocated `SoaEvent → IpcSlot`, no heap on hot path
- `DdsQosEnforcer`: per-topic Reliability, Deadline, LatencyBudget, History enforcement
- `DeadSubscriberMonitor`: heartbeat TTL 2000ms, `ScanAndEvict()`, on-death callback

### Phase 5 — Software Engineering ✅
| Check | Result |
|-------|--------|
| `try/catch/throw` | **0** (all production files) |
| `std::mutex` | **0** |
| `new` / `malloc` | **0** |
| `std::vector` / `std::map` | **0** |
| Doxygen headers | **17 / 17** files |
| Include guards | **9 / 9** headers |
| `static_assert` on IPC structs | **6** assertions |
| Unit test cases | **118** (GoogleTest, MC/DC) |

---

## Performance (NXP S32G EVB, Cortex-A53 @ 1.0 GHz, QNX 8.0, `-O2`)

| Operation | Measured | Budget |
|-----------|---------|--------|
| `IpcBridge::Send()` full path with E2E | < 15 μs | < 50 μs |
| `IamSecurityController::Authorise()` (64 policies) | < 2 μs | < 10 μs |
| `SafetyArbitrator::Arbitrate()` (8 domains) | < 800 μs | < 2 ms |
| `SafetyArbitrator::ExecuteTransition()` | < 5 ms | **< 50 ms** (ISO 26262) |
| `RateLimiter::Admit()` CAS fast path | < 200 ns | < 1 μs |
| E2E CRC-16/ARC (148 bytes) | < 3 μs | < 10 μs |

---

## Build Instructions

### Requirements
- CMake ≥ 3.16
- GCC ≥ 11 or Clang ≥ 14 (host build)
- QNX SDP 8.0 (target build, see `cmake/Toolchain-QNX.cmake`)
- GoogleTest (for tests)
- lcov + genhtml (for coverage reports)
- cppcheck (for static analysis)

### Library only
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### With unit tests (GCC)
```bash
cmake .. -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./soa_gateway_tests --gtest_output=xml:test_core.xml
./safety_arbitrator_tests --gtest_output=xml:test_arbitrator.xml
```

### With MC/DC coverage report
```bash
cmake .. -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON
make -j$(nproc)
./soa_gateway_tests && ./safety_arbitrator_tests
make coverage
# Open: build/coverage_html/index.html
```

### Static analysis (AUTOSAR/MISRA)
```bash
make analyze
# Report: build/cppcheck_report.xml  (zero errors required)
```

### QNX cross-compilation (NXP S32G target)
```bash
source /path/to/qnx800/qnxsdp-env.sh
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-QNX.cmake \
  -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Cortex-M7 bare-metal reference (AUTOSAR R25-11)
```bash
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-S32G-M7.cmake \
  -DGCC_ARM_PATH=/path/to/arm-gnu-toolchain-13 \
  -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## Repository Structure

```
soa_gateway/
├── include/                    Production header files
│   ├── SoaServiceManager.hpp   Core: SD lifecycle, SPSC dispatch
│   ├── NetworkAdapter.hpp      Abstract: SOME/IP & DDS interface
│   ├── IamSecurityController.hpp  Security: RBAC + audit log
│   ├── IpcBridge.hpp           IPC: shared SRAM + E2E Profile 5
│   ├── HseAdapter.hpp          Security: NXP HSE MU driver
│   ├── RateLimiter.hpp         Security: Token Bucket anti-DDoS
│   ├── DdsQosPolicy.hpp        Middleware: DDS QoS enforcement
│   ├── DeadSubscriberMonitor.hpp  Middleware: heartbeat TTL
│   └── SafetyArbitrator.hpp    Safety: ASIL-D state machine
├── src/                        Production source files (8 modules)
├── tests/
│   ├── test_soa_gateway.cpp    Core module tests (60+ cases)
│   └── test_safety_arbitrator.cpp  ASIL-D MC/DC tests (52 cases)
├── cmake/
│   ├── Toolchain-QNX.cmake     QNX SDP 8.0 cross-compilation
│   └── Toolchain-S32G-M7.cmake ARM bare-metal (Cortex-M7)
├── docs/
│   └── architecture.md         Full architecture deep dive
├── .github/
│   ├── workflows/ci.yml        Build · Test · Analyze · Compliance CI
│   ├── ISSUE_TEMPLATE/         Bug report template
│   └── PULL_REQUEST_TEMPLATE.md
├── CMakeLists.txt
├── LICENSE                     norxs Reference Implementation License v1.0
├── CHANGELOG.md                Full version history
├── CONTRIBUTING.md             Coding standards & contribution guide
└── README.md                   This file
```

---

## Documentation

- **[Architecture Deep Dive](docs/architecture.md)** — System context, data flow,
  concurrency model, E2E protocol, memory layout, performance figures,
  standards traceability
- **[CHANGELOG](CHANGELOG.md)** — Complete feature history for v1.0.0
- **[CONTRIBUTING](CONTRIBUTING.md)** — Coding standards, AUTOSAR compliance
  checklist, PR process
- **[LICENSE](LICENSE)** — norxs Reference Implementation License v1.0

---

## Commercial Licensing & Services

This reference implementation is published under the
**norxs Reference Implementation License v1.0**.
Commercial use requires a separate license agreement.

**norxs Technology LLC** offers:
- Full production source rights for ASIL-D deployment
- ISO 26262 safety evidence package (FMEA, FTA, DFA)
- UN R155 / ISO 21434 cybersecurity artifact package
- ASPICE process documentation
- Long-term engineering support and maintenance

**Contact:** https://norxs.com

---

## Namespace

All symbols reside in `norxs::soa`.

## Standards

ISO 26262 · ISO/SAE 21434 · UN R155 · UN R156 · ASPICE · AUTOSAR C++14 · POSIX

---

*(c) 2026 norxs Technology LLC. All rights reserved.*
