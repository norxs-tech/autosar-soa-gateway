# Security Policy

**norxs Technology LLC** operates a security assurance program conformant with
**OpenChain ISO/IEC 18974:2023** and aligned with the **NIST Cybersecurity
Framework (CSF) 2.0**. This document defines how to report vulnerabilities in
this repository and what you can expect from us.

---

## Supported Versions

| Version | Supported |
|---------|-----------|
| 1.0.x   | ✅ Active security maintenance |
| < 1.0   | ❌ Pre-release, not supported |

---

## Reporting a Vulnerability

**Please do NOT open a public GitHub issue for security vulnerabilities.**

Report privately through one of these channels:

1. **Email (preferred):** `contact@norxs.com` — use subject prefix `[SECURITY]`
2. **GitHub Private Vulnerability Reporting:** via the *Security* tab →
   *Report a vulnerability* (if enabled on this repository)

### What to include

- Affected file(s), module(s), and version / commit hash
- Vulnerability class (e.g., CWE identifier if known)
- Reproduction steps or proof-of-concept
- Impact assessment in the context of the target platform
  (NXP S32G, QNX 8.0, Cortex-M7 AUTOSAR R25-11)
- Your preferred credit name for the advisory (optional)

### Our response commitments (Coordinated Vulnerability Disclosure)

| Stage | Target |
|-------|--------|
| Acknowledgement of report | **≤ 3 business days** |
| Initial triage & severity assignment (CVSS v3.1) | ≤ 7 business days |
| Status updates to reporter | At least every 14 days |
| Fix or documented mitigation for confirmed issues | ≤ 90 days (severity-dependent) |
| Coordinated public disclosure | After fix release, agreed with reporter |

We follow ISO/IEC 29147 (vulnerability disclosure) and ISO/IEC 30111
(vulnerability handling) practices. We will not pursue legal action against
good-faith security research conducted within this policy's scope.

---

## Scope

In scope:
- All production code in `include/` and `src/`
- Build system and CI configuration (`CMakeLists.txt`, `.github/workflows/`)
- Documentation that could lead to insecure deployment if followed

Out of scope:
- Vulnerabilities in third-party dependencies (report upstream; see `NOTICE.md`
  and `sbom/` — we still appreciate a heads-up so we can track them)
- Issues requiring physical access to a debug port already considered in the
  threat model (see `docs/architecture.md` §8)
- Denial of service against the public GitHub repository itself

---

## Security Design Context

This reference implementation enforces a defense-in-depth posture documented in
`docs/architecture.md` §8 (Cybersecurity Architecture):

- **UN R155 / ISO/SAE 21434** threat-model-driven controls:
  RBAC default-deny firewall (`IamSecurityController`), per-source token-bucket
  rate limiting (`RateLimiter`), SecOC AES-128-CMAC and X.509 verification via
  the NXP HSE (`HseAdapter`)
- **AUTOSAR E2E Profile 5** integrity protection on the cross-core IPC path
- **Fail-secure defaults:** authorization table exhaustion and rate-limiter
  table exhaustion both deny, never allow

Known limitations of the *reference* configuration (vs. a production
engagement) are listed in `docs/compliance/openchain-iso18974.md` §Known Gaps.

---

## Known Vulnerabilities Process (ISO/IEC 18974 §3.1.4 / §3.1.5)

- Newly disclosed vulnerabilities in components listed in our SBOM
  (`sbom/norxs-soa-gateway.spdx.json`) are monitored against the
  NVD / GitHub Advisory Database.
- Confirmed vulnerabilities affecting released versions are communicated via
  GitHub Security Advisories on this repository and noted in `CHANGELOG.md`.

---

## PGP / Encrypted Reports

If you require encrypted communication, request our current PGP key via
`contact@norxs.com` before sending sensitive details.

---

*© 2026 norxs Technology LLC · Wyoming, US · https://norxs.com*
