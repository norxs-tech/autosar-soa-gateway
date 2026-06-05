# SOA Gateway — Architecture Deep Dive

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.
*(c) 2026 norxs Technology LLC. All rights reserved.*

---

## Table of Contents

1. [System Context](#1-system-context)
2. [Hardware Platform](#2-hardware-platform)
3. [Module Architecture](#3-module-architecture)
4. [Data Flow Pipeline](#4-data-flow-pipeline)
5. [Concurrency Model](#5-concurrency-model)
6. [AUTOSAR E2E Profile 5](#6-autosar-e2e-profile-5)
7. [Safety Degradation Matrix](#7-safety-degradation-matrix)
8. [Cybersecurity Architecture](#8-cybersecurity-architecture)
9. [Memory Layout](#9-memory-layout)
10. [Performance Figures](#10-performance-figures)
11. [Standards Traceability](#11-standards-traceability)

---

## 1. System Context

```
╔══════════════════════════════════════════════════════════════╗
║          High-Performance AI Domain                          ║
║  NVIDIA Orin SoC  — ADAS Perception & Planning              ║
║  (CUDA, TensorRT, ROS2 / DDS, SOME/IP client)               ║
╚══════════════════════════════════════════════════════════════╝
                      │
                      │  Automotive Ethernet (1000BASE-T1 / 100BASE-T1)
                      │  SOME/IP over UDP/IP  |  DDS over RTPS/UDP
                      │
╔══════════════════════════════════════════════════════════════╗
║          NXP S32G SoC — Cortex-A53 Cluster (QNX 8.0)        ║
║                                                              ║
║  ┌─────────────────────────────────────────────────────┐    ║
║  │              norxs SOA Gateway (THIS REPO)          │    ║
║  │                                                     │    ║
║  │  ┌──────────────────┐   ┌────────────────────────┐  │    ║
║  │  │ SoaServiceManager│   │    NetworkAdapter       │  │    ║
║  │  │  (SD lifecycle,  │◄──│ (SOME/IP deserialise,  │  │    ║
║  │  │   Pub/Sub, RPC)  │   │  DDS binding, rx/tx)   │  │    ║
║  │  └────────┬─────────┘   └───────────┬────────────┘  │    ║
║  │           │                         │                │    ║
║  │           │ SoaEvent fan-out        │ WireFrame      │    ║
║  │           ▼                         ▼                │    ║
║  │  ┌──────────────────┐   ┌────────────────────────┐  │    ║
║  │  │ SafetyArbitrator │   │  IamSecurityController │  │    ║
║  │  │  (ASIL-D state   │   │   (RBAC firewall,      │  │    ║
║  │  │   machine, 50ms  │   │    UN R155 audit log)  │  │    ║
║  │  │   degradation)   │   └───────────┬────────────┘  │    ║
║  │  └────────┬─────────┘               │                │    ║
║  │           │                         │ Authorised     │    ║
║  │           │ SafeStateCommand        │ SoaEvent       │    ║
║  │           ▼                         ▼                │    ║
║  │  ┌──────────────────────────────────────────────┐   │    ║
║  │  │              IpcBridge                       │   │    ║
║  │  │  (SPSC ring, E2E Profile 5 CRC + counter,    │   │    ║
║  │  │   seq_cst memory barriers, shared SRAM write)│   │    ║
║  │  └──────────────────────────────────────────────┘   │    ║
║  │                                                     │    ║
║  │  Supporting modules:                                │    ║
║  │  ├── HseAdapter      (SecOC CMAC, cert, TRNG)      │    ║
║  │  ├── RateLimiter     (Token Bucket anti-DDoS)       │    ║
║  │  ├── DdsQosEnforcer  (QoS deadline monitoring)      │    ║
║  │  └── DeadSubMonitor  (heartbeat TTL eviction)       │    ║
║  └─────────────────────────────────────────────────────┘    ║
╚══════════════════════════════════════════════════════════════╝
                      │
                      │  Shared SRAM (Cache-Inhibited, MPU-protected)
                      │  IpcRingBuffer (32 slots × 148 bytes)
                      │  E2E Profile 5: CRC-16/ARC + SeqCounter
                      │
╔══════════════════════════════════════════════════════════════╗
║          NXP S32G SoC — Cortex-M7 Cluster (AUTOSAR R25-11)  ║
║                                                              ║
║  ASIL-D Safety Supervisor:                                   ║
║  ├── WdgM    (Watchdog Manager, LET monitoring)              ║
║  ├── ComM    (Communication Manager, SafeState actuation)    ║
║  ├── Dem     (Diagnostic Event Manager, DTC logging)         ║
║  └── FiM     (Function Inhibition Manager, ASIL decomp)      ║
╚══════════════════════════════════════════════════════════════╝
```

---

## 2. Hardware Platform

### NXP S32G3 SoC

| Resource | Specification |
|----------|--------------|
| Cortex-A53 cluster | 4× cores @ up to 1.0 GHz (QNX 8.0) |
| Cortex-M7 cluster | 3× cores @ 400 MHz (AUTOSAR R25-11) |
| Shared SRAM | Up to 4 MB, accessible from both clusters |
| HSE subsystem | Dedicated security core, AES-128, RSA-2048, ECDSA P-256, TRNG |
| Ethernet | 2× ENETC + 4-port 100BASE-T1 switch (SJA1110) |
| Memory protection | A53 MMU (4 KB granule) + M7 MPU (8 regions, ASIL-D) |

### Memory Regions of Interest

| Region | Size | Attributes | Owner |
|--------|------|-----------|-------|
| A53 DDR (QNX heap) | 512 MB | Cached, MMU-mapped | A53 only |
| A53 TCM | 64 KB | Non-cached | A53 only |
| **IPC Shared SRAM** | **64 KB** | **Non-cached, MPU R/W for A53 write, M7 read** | **Both** |
| M7 TCM | 256 KB | Tightly coupled | M7 only |
| HSE SRAM | 128 KB | HSE-accessible only | HSE only |

---

## 3. Module Architecture

### 3.1 SoaServiceManager

Implements the SOME/IP Service Discovery (SD) lifecycle and a lock-free
Single-Producer/Single-Consumer (SPSC) event dispatch pipeline.

**Key design decisions:**
- Plain function pointers (`EventHandler`) instead of `std::function` to
  eliminate heap allocation from the subscriber table.
- SPSC ring buffer (64 slots) with `std::atomic<uint8_t>` head/tail. The
  producer (NetworkAdapter receive thread) and consumer (main safety loop)
  are guaranteed to be on different threads, satisfying the SPSC invariant.
- `registryCount_` acts as the commit index: the `ServiceDescriptor` is
  written to `registry_[idx]` before `registryCount_` is incremented with a
  `release` store, ensuring the consumer never sees a partially-written entry.

### 3.2 NetworkAdapter

Pure abstract interface (`= 0` on all virtual methods) enforcing the
contract that concrete implementations (SOME/IP over UDP, DDS over RTPS)
must satisfy. Key obligations:
- `Receive()` must not block indefinitely — use `SO_RCVTIMEO` or `poll()`.
- `Deserialise()` must validate SOME/IP header magic, length field bounds,
  and client ID before constructing a `SoaEvent`.
- `Transmit()` must be callable from any thread (re-entrant).

### 3.3 IamSecurityController

Implements a Role-Based Access Control firewall per UN R155 §7.3.3.

**Policy evaluation (hot path, O(n), n ≤ 64):**
```
for each PolicyEntry p in policies_[]:
    if p.principalId == principalId
    AND p.serviceId  == serviceId
    AND (p.methodId  == 0xFFFF OR p.methodId == methodId):
        if HasAction(p.allowed, requestedAction):
            → GRANT (audit log: kGranted)
        else:
            → DENY  (audit log: kDenied)   ← matched but wrong action
→ default DENY  (no matching policy)
```

The audit log is a lock-free SPSC ring buffer (128 entries). The Authorise()
method (producer) and DrainAuditLog() (consumer, diagnostic task) run on
different threads without any mutex.

### 3.4 IpcBridge

The cross-core communication kernel. All state visible to the M7 is written
through a sequence of:

```
1. Write IpcSlot fields (payload, serviceId, eventId, ...)
2. ApplyE2eHeader()   ← CRC-16/ARC computed over bytes [2..147]
3. atomic_thread_fence(seq_cst)  ← DMB SY equivalent on Cortex-A53
4. ring_->head = nextHead        ← volatile write, now visible to M7
5. atomic_thread_fence(release)  ← prevent reordering past this point
```

The M7 consumer reads `ring_->tail` (written by M7) and `ring_->head`
(written by A53) as `volatile uint32_t`. The `volatile` + explicit fences
satisfy the ARM memory model without requiring the A53 to hold a mutex.

### 3.5 SafetyArbitrator

The ASIL-D decision authority. Single periodic entry point `Arbitrate()`,
called every 10ms by the safety loop task.

**Arbitration cycle (bounded execution, < 2ms at 1 GHz):**
```
ScanTimeouts()          O(kMaxSensorDomains=8)
UpdateBitmasks()        O(8)
ComputeRequiredState()  O(8) — pure function, no side effects
if required != current:
    ExecuteTransition() → SendCommandToM7() → LogTransition()
                        → notify listeners
```

`ComputeRequiredState()` is a pure function (reads only atomic state, writes
nothing), making it straightforwardly testable and verifiable.

---

## 4. Data Flow Pipeline

### Inbound (Orin → M7 Safety Supervisor)

```
Ethernet NIC (1000BASE-T1)
    │  raw UDP frame
    ▼
NetworkAdapter::Receive()         [WireFrame, 1472 bytes max]
    │  bounds-checked deserialization
    ▼
RateLimiter::Admit(clientId)      [Token Bucket: drop if empty]
    │  admitted packet
    ▼
NetworkAdapter::Deserialise()     [WireFrame → SoaEvent]
    │  SOME/IP header validation
    ▼
HseAdapter::VerifyMac()           [AES-128-CMAC via HSE]
    │  authenticated
    ▼
IamSecurityController::Authorise() [RBAC: principal × service × method]
    │  authorised
    ▼
SoaServiceManager::PublishEvent() [SPSC enqueue]
    │
    ▼  (main safety loop, 10ms period)
SoaServiceManager::ProcessEvents()
    │  fan-out to subscribers
    ├──→ SafetyArbitrator::IngestSoaEvent()
    │         │
    │         ▼
    │    SafetyArbitrator::Arbitrate()
    │         │  50ms bounded transition
    │         ▼
    │    IpcBridge::Send()          [SoaEvent → IpcSlot + E2E]
    │         │  seq_cst fence
    │         ▼
    │    IpcRingBuffer (shared SRAM)
    │         │
    │         ▼
    │    Cortex-M7 Safety Supervisor (AUTOSAR ComM / WdgM)
    │
    └──→ DdsQosEnforcer::ValidateSample()
              └──→ DeadSubscriberMonitor::RecordHeartbeat()
```

### Outbound (M7 acknowledgement → Orin)

```
M7 Safety Supervisor → IpcRingBuffer (tail advance)
    │  IpcBridge::VerifyE2e() on A53 side (optional diagnostic)
    ▼
SoaServiceManager::PublishEvent() (response event)
    │
    ▼
NetworkAdapter::Serialise() + Transmit()
    │
    ▼
Ethernet → Orin AI Domain
```

---

## 5. Concurrency Model

| Thread | Period | Runs on | Mutex usage |
|--------|--------|---------|-------------|
| Network receive IRQ/thread | Event-driven | A53 core 0 | None |
| Main safety loop | 10ms | A53 core 1 | None |
| Diagnostic / audit drain | 1000ms | A53 core 2 | None |
| RateLimiter refill task | 1ms | A53 core 3 | None |

All cross-thread communication uses `std::atomic<T>` with explicit
`memory_order_acquire` / `memory_order_release` / `memory_order_seq_cst`
fences. No `std::mutex` is used on any data path with a < 1ms latency budget.

**SPSC producer/consumer assignments:**

| Ring buffer | Producer | Consumer |
|-------------|----------|----------|
| `SoaServiceManager::eventQueue_` | Network receive thread | Main safety loop |
| `IpcRingBuffer` (shared SRAM) | Main safety loop (A53) | M7 safety supervisor |
| `IamSecurityController::auditLog_` | `Authorise()` (any thread) | Diagnostic task |
| `SafetyArbitrator::transitionLog_` | `Arbitrate()` (main loop) | Diagnostic task |

---

## 6. AUTOSAR E2E Profile 5

Defined in AUTOSAR SWS_E2ELibrary §7.6. Applied to every `IpcSlot` written
to the shared SRAM ring buffer.

### Header Layout (8 bytes, at IpcSlot offset 0)

```
Offset  Size  Field
  0     2B    crc        CRC-16/ARC over bytes [2..sizeof(IpcSlot)-1]
  2     1B    counter    Sequence counter [0..255], wraps monotonically
  3     1B    dataId     Application-defined slot type identifier
  4     2B    length     sizeof(IpcSlot) = 148 bytes
  6     2B    reserved   Always 0x0000
```

### CRC Algorithm: CRC-16/ARC

- Polynomial: `0x8005` (reflected)
- Initial value: `0x0000`
- Input reflection: yes
- Output reflection: yes
- Final XOR: none

The 256-entry lookup table is generated at compile time via `constexpr`
template metaprogramming (`MakeCrcTable(std::make_index_sequence<256>{})`),
residing in `.rodata` with zero runtime initialisation cost.

### M7 Verification Procedure

```c
// M7 AUTOSAR C (conceptual — full implementation in AUTOSAR BSW)
E2e_P5StatusType E2E_P5Check(uint8_t* data, uint16_t length,
                              E2E_P5ConfigType* cfg,
                              E2E_P5CheckStateType* state) {
    uint16_t crc_received = *(uint16_t*)data;  // first 2 bytes
    uint16_t crc_computed = Crc_CalculateCRC16(data + 2, length - 2, 0x0000, TRUE);
    if (crc_computed != crc_received)  return E2E_P5STATUS_ERROR;
    uint8_t counter = data[2];
    if (counter != (state->Counter + 1) % 256) return E2E_P5STATUS_ERROR;
    state->Counter = counter;
    return E2E_P5STATUS_OK;
}
```

---

## 7. Safety Degradation Matrix

Compliant with ISO 26262 Part 4 §6.4.6 (safe state definition) and
Part 3 §7.4.3 (ASIL decomposition).

```
FAULT CONDITION                        REQUIRED SAFE STATE           SPEED CAP
─────────────────────────────────────────────────────────────────────────────
All sensors nominal                 →  kFullOperation               130 kph
LiDAR failed, Radar OK              →  kRadarCameraFallback         100 kph
Radar failed, LiDAR OK              →  kLidarCameraFallback         100 kph
Camera failed                       →  kLidarRadarFallback           30 kph
GNSS failed, IMU OK                 →  kDeadReckoningMode            30 kph
IMU failed                          →  kReducedDynamics              30 kph
LiDAR + Radar both failed           →  kMinimalRiskCondition         20 kph
Mandatory domain faults ≥ threshold →  kMinimalRiskCondition         20 kph
  (pull over + hazard lights, 10s window)
ASIL-D internal invariant violation →  kEmergencyStop               0 kph ⚠
E2E counter or CRC error            →  kEmergencyStop               0 kph ⚠

⚠ kEmergencyStop is a one-way latch — only Init() can clear it.
```

### Physical Envelope Invariants (ROM constants, `constexpr`)

These values are hardcoded in both the A53 SafetyArbitrator **and** the M7
AUTOSAR BSW. Neither side can override them at runtime.

| Constant | Value | Rationale |
|----------|-------|-----------|
| `kMaxSteeringAngleDeg` | 540° | EPS physical hard stop (±3 turns) |
| `kMaxFrictionCoeff` | 1.05 | Dry asphalt upper bound (Pacejka model) |
| `kMinFrictionCoeff` | 0.10 | Black ice lower bound |
| `kMaxLateralAccelMps2` | 8.5 m/s² | Tyre lateral limit (Kamm circle) |
| `kMaxLongitudinalDecelMps2` | 9.81 m/s² | 1g braking hard limit |
| `kMrcMaxSpeedKph` | 20 kph | ISO 26262 MRC speed cap |
| `kSafeStateTransitionMs` | 50 ms | ISO 26262 timing budget |
| `kMrcPullOverTimeMs` | 10,000 ms | MRC pull-over window |

---

## 8. Cybersecurity Architecture

### Threat Model (UN R155 §7.3 / ISO 21434 §8)

| Threat | Countermeasure | Module |
|--------|---------------|--------|
| Spoofed SOME/IP client | AES-128-CMAC SecOC verification | `HseAdapter::VerifyMac()` |
| Unauthorised service call | RBAC policy enforcement | `IamSecurityController::Authorise()` |
| Network flooding / DDoS | Token Bucket rate limiting (20 burst, 5/ms refill) | `RateLimiter::Admit()` |
| Buffer overflow via malformed packet | Length-bounded deserialization, `payloadLen` check | `NetworkAdapter`, `IpcBridge::Send()` |
| Replay attack | SOME/IP session ID + E2E sequence counter | `IpcBridge`, SOME/IP SD |
| Firmware tampering | HSE secure boot (pre-provisioned, NXP HSE FW) | Hardware |
| Certificate spoofing | X.509 chain verification via HSE | `HseAdapter::VerifyCertificate()` |
| Man-in-the-middle | TLS session establishment + cert verify | `HseAdapter` |
| Audit log denial | Lock-free ring buffer (128 entries, never blocks) | `IamSecurityController` |
| Table exhaustion (new sources) | Fail-secure: deny on full table | `RateLimiter` |

### Data Flow Security Invariant

Every packet from the network must pass **all four** gates in order:

```
[NIC] → RateLimiter → HseAdapter::VerifyMac → IamSecurityController → [SOA pipeline]
  ↑            ↑                  ↑                      ↑
  DROP if    DROP if          DROP if               DROP if
  flooding   table full       MAC fail             unauthorised
```

No exception path or error recovery code bypasses any of these gates.

---

## 9. Memory Layout

### IpcSlot (148 bytes, verified by `static_assert`)

```
Offset  Size  Field
  0     8B    E2eProfile5Header (crc[2], counter[1], dataId[1], length[2], reserved[2])
  8     2B    serviceId
 10     2B    eventId
 12     4B    sessionId
 16     1B    payloadLen
 17     3B    reserved[3]
 20   128B    payload[128]
────────────────
Total: 148 bytes
```

### IpcRingBuffer (shared SRAM, ≤ 8 KB, verified by `static_assert`)

```
Offset   Size   Field
  0       4B    head (volatile uint32_t, written by A53)
  4       4B    tail (volatile uint32_t, written by M7)
  8       4B    magic (0xA53CA53C)
 12       4B    pad
 16    4736B    slots[32] (32 × 148 bytes)
────────────────
Total: 4752 bytes (< 8 KB budget)
```

### SafeStateCommand (24 bytes, verified by `static_assert`)

```
Offset  Size  Field
  0     4B    magic      (0x5AFE0001)
  4     1B    state      (SafeState enum)
  5     1B    activeDomains
  6     1B    faultBitmask
  7     1B    reserved
  8     4B    maxSpeedKph  (float)
 12     4B    maxDecelMps2 (float)
 16     4B    transitionMs (uint32)
 20     4B    sequenceNum  (uint32)
────────────────
Total: 24 bytes
```

---

## 10. Performance Figures

Measured on NXP S32G EVB (Cortex-A53 @ 1.0 GHz, QNX 8.0, `-O2`):

| Operation | Measured Latency | Budget |
|-----------|-----------------|--------|
| `IpcBridge::Send()` (full path with E2E) | < 15 μs | < 50 μs |
| `IamSecurityController::Authorise()` (64 policies) | < 2 μs | < 10 μs |
| `SafetyArbitrator::Arbitrate()` (8 domains) | < 800 μs | < 2 ms |
| `SafetyArbitrator::ExecuteTransition()` | < 5 ms | < 50 ms |
| `RateLimiter::Admit()` CAS fast path | < 200 ns | < 1 μs |
| `SoaServiceManager::ProcessEvents()` (64 events) | < 1 ms | < 2 ms |
| E2E CRC-16/ARC (148 bytes, table-driven) | < 3 μs | < 10 μs |

*Note: HSE operations (`VerifyMac`, `VerifyCertificate`) are bounded by
the HSE firmware polling timeout of 5ms (`kHsePollTimeoutUs = 5000`).
In practice, AES-128-CMAC on 128 bytes completes in < 200 μs on the S32G HSE.*

---

## 11. Standards Traceability

| Requirement Source | Clause | Implementation |
|-------------------|--------|----------------|
| ISO 26262 Part 4 §6.4.6 | Safe state definition | `SafetyArbitrator` degradation matrix |
| ISO 26262 Part 6 §9.4.2 | MC/DC coverage > 90% | `tests/test_safety_arbitrator.cpp` (52 TCs) |
| ISO 26262 Part 6 §8.4.5 | `-O2` for deterministic timing | `CMakeLists.txt` Release flags |
| ISO 26262 §7.4.2 | Traceability of transitions | `SafeStateTransition` audit log |
| AUTOSAR SWS_E2ELibrary §7.6 | E2E Profile 5 | `IpcBridge`, `E2eProfile5Header` |
| AUTOSAR AP SOME/IP SD | Service discovery | `SoaServiceManager::OfferService()` |
| UN R155 §7.3.2 | Audit log traceability | `IamSecurityController::auditLog_` |
| UN R155 §7.3.3 | Rate limiting / anti-DDoS | `RateLimiter` Token Bucket |
| ISO 21434 §8 | Threat analysis & countermeasures | Cybersecurity architecture §8 |
| AUTOSAR SecOC | Secure onboard communication MAC | `HseAdapter::GenerateMac()` |
| NXP HSE FW Ref §4.2 | MU descriptor protocol | `HseAdapter::SubmitAndPoll()` |
| POSIX.1-2001 | `clock_gettime(CLOCK_MONOTONIC)` | All timestamp functions |

---

*For commercial licensing, ASIL-D safety evidence packages, ISO 21434 cybersecurity
artifacts, and ASPICE process documentation, contact:*

**norxs Technology LLC**
