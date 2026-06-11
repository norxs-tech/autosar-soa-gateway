# OpenChain ISO/IEC 18974:2023 — Security Assurance Mapping

ISO/IEC 18974 defines the key requirements of a quality **open source security
assurance program** — i.e., a repeatable process for identifying and responding
to known vulnerabilities in software components. This document maps the
specification to the practices applied to this repository.

| ISO/IEC 18974 §3 Requirement | Implementation in this repository |
|---|---|
| **3.1.1** Documented procedure for security assurance | `SECURITY.md` (public CVD policy) + this mapping document |
| **3.1.2** Roles & competence | Security contact: engineering lead (TÜV Automotive Cybersecurity Professional, ISO/SAE 21434); single point of contact `contact@norxs.com` |
| **3.1.3** Awareness of staff | `CONTRIBUTING.md` mandates the secure-coding ruleset (no heap, no exceptions, fail-secure defaults) for every PR; CI enforces it mechanically |
| **3.1.4** Identifying known vulnerabilities | Component inventory in SPDX SBOM (`sbom/`); test/tool dependencies monitored against NVD & GitHub Advisory Database; GitHub Dependabot alerts enabled on the repository |
| **3.1.5** Responding to known vulnerabilities | CVD process in `SECURITY.md`: triage ≤ 7 days, CVSS v3.1 scoring, fix ≤ 90 days, GitHub Security Advisory + `CHANGELOG.md` entry |
| **3.2.1** Method for external reports | Private channels: `[SECURITY]` email + GitHub Private Vulnerability Reporting (`SECURITY.md` §Reporting) |
| **3.2.2** Resourcing | Security response is part of release-blocking duties; timelines committed publicly in `SECURITY.md` |
| **3.3.1** Software assurance artifacts | Per release: SBOM, `docs/verification-report.md` (test/static-analysis evidence), CI artifacts (cppcheck XML, GoogleTest XML, coverage HTML) |

## Technical Controls Relevant to Security Assurance

| Control | Module | Standard hook |
|---|---|---|
| Default-deny RBAC with audit log | `IamSecurityController` | UN R155 §7.3.2 |
| Per-source token-bucket rate limiting, fail-secure on table exhaustion | `RateLimiter` | UN R155 §7.3.3 |
| SecOC AES-128-CMAC, X.509 chain verification, TRNG via HSE | `HseAdapter` | ISO/SAE 21434 |
| E2E Profile 5 (CRC-16/ARC + sequence counter) on cross-core IPC | `IpcBridge` | AUTOSAR SWS E2E |
| No dynamic allocation / no exceptions / lock-free atomics only | all production code | AUTOSAR C++14, CWE-401/-248 class elimination |

## Known Gaps (Reference Implementation vs. Production)

Disclosed for transparency — a production engagement closes these:

1. `HseAdapter` targets the NXP HSE MU interface; on host builds the HSE is
   stubbed, so cryptographic verification is exercised structurally, not
   against silicon.
2. Fuzz testing (e.g., libFuzzer on the SOME/IP deserialization path) is not
   yet part of CI — planned (see `CHANGELOG.md` Unreleased).
3. No signed releases / provenance attestation (SLSA) yet — planned.

---
*© 2026 norxs Technology LLC.*
