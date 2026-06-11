# NIST Cybersecurity Framework (CSF) 2.0 — Mapping

This document maps the six NIST CSF 2.0 Functions to the engineering and
process controls applied to this repository and its target system (NXP S32G
SOA Gateway). It demonstrates how norxs applies the CSF at both the
**organizational** level and the **embedded product** level.

## GOVERN (GV)

| Category | Implementation |
|---|---|
| GV.OC / GV.RM — Context & risk strategy | Product risk derived from ISO/SAE 21434 TARA; safety risk from ISO 26262 HARA; documented in `docs/architecture.md` §8 and §11 |
| GV.PO — Policy | Public security policy `SECURITY.md`; OSS policy per `docs/compliance/openchain-iso5230.md` |
| GV.SC — Supply chain risk | SPDX SBOM (`sbom/`); zero third-party runtime dependencies; CI `supply-chain` job verifies SBOM/source consistency |

## IDENTIFY (ID)

| Category | Implementation |
|---|---|
| ID.AM — Asset management | Module inventory (`README.md` §Module Inventory); SBOM with per-file SHA-1 |
| ID.RA — Risk assessment | UN R155 attack surfaces enumerated (SOME/IP, DDS, IPC, debug); degradation matrix maps hazards to safe states |

## PROTECT (PR)

| Category | Implementation |
|---|---|
| PR.AA — Identity & access control | `IamSecurityController` RBAC (principal × service × method), default-deny |
| PR.DS — Data security | `HseAdapter` AES-128-CMAC SecOC, X.509 verification; E2E Profile 5 integrity on IPC |
| PR.PS — Platform security | MPU-protected shared SRAM, write-only A53 access, `.rodata` physical invariants; `-fno-exceptions -fno-rtti`, zero heap |
| PR.IR — Infrastructure resilience | `RateLimiter` token bucket anti-DDoS; fail-secure on resource exhaustion |

## DETECT (DE)

| Category | Implementation |
|---|---|
| DE.CM — Continuous monitoring | `DeadSubscriberMonitor` heartbeat TTL; `SafetyArbitrator` sensor-health timeout scan; 128-entry UN R155 security audit log |
| DE.AE — Adverse event analysis | E2E violations escalate to `kEmergencyStop`; transition audit trail (`DrainTransitionLog`) for forensics |

## RESPOND (RS)

| Category | Implementation |
|---|---|
| RS.MA — Incident management | Product level: ASIL-D degradation matrix, ≤ 50 ms bounded transitions. Org level: CVD process in `SECURITY.md` |
| RS.CO — Communication | GitHub Security Advisories + `CHANGELOG.md`; reporter coordination per `SECURITY.md` |

## RECOVER (RC)

| Category | Implementation |
|---|---|
| RC.RP — Recovery plan | Defined recovery paths in the safe-state machine (fallback → restore on health recovery; E-stop latch requires authenticated service reset by design) |
| RC.CO — Recovery communication | Release notes / advisories communicate fixed versions and mitigation steps |

---
*© 2026 norxs Technology LLC.*
