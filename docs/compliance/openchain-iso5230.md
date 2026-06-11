# OpenChain ISO/IEC 5230:2020 — Conformance Mapping

**Scope of program:** norxs Technology LLC — all public repositories under the
`norxs-tech` GitHub organization, including this repository
(`autosar-soa-gateway`).

ISO/IEC 5230 defines the key requirements of a quality **open source license
compliance program**. This document maps each specification section to the
concrete artifact or process implemented for this repository.

| ISO/IEC 5230 §3 Requirement | Implementation in this repository |
|---|---|
| **3.1.1** Documented policy | norxs Open Source Policy (internal); public summary in `NOTICE.md` and this document |
| **3.1.2** Competence | Program owner is TÜV-certified FuSa Expert / Cybersecurity Professional; OSS license review is a defined responsibility of the engineering lead |
| **3.1.3** Awareness | `CONTRIBUTING.md` requires contributors to confirm rights to contribute and binds contributions to the inbound license terms |
| **3.1.4** Program scope | All code in `include/`, `src/`, `tests/`, build & CI configuration |
| **3.1.5** License obligations | Obligations of all identified components are recorded in `NOTICE.md` (attribution, license texts referenced) |
| **3.2.1** External inquiries | Public contact published: `contact@norxs.com` (also on https://norxs.com and in `README.md`) |
| **3.2.2** Resourcing | Compliance tasks are part of the release checklist; CI job `supply-chain` blocks merges if SBOM/NOTICE drift from the source tree |
| **3.3.1** Bill of materials | Machine-readable SPDX 2.3 SBOM: `sbom/norxs-soa-gateway.spdx.json`, regenerated and verified per release |
| **3.3.2** License compliance | Production code: 100% norxs-owned (LicenseRef-norxs-RI-1.0). Test-only deps (BSD-3-Clause GoogleTest) are not distributed; obligations satisfied via `NOTICE.md` |
| **3.4.1** Compliance artifacts | `LICENSE`, `NOTICE.md`, `sbom/*.spdx.json` archived with every tagged release |
| **3.5.1** Contributions policy | `CONTRIBUTING.md` §License Agreement defines inbound=outbound terms and employer-authorization requirement |
| **3.6.1** Conformance claim | norxs Technology LLC self-certifies conformance; announcement published on https://norxs.com/news |

## Per-Release Checklist

1. Regenerate SBOM (`sbom/`) — file list and SHA-1 checksums must match the tag.
2. Re-verify `NOTICE.md` against any new test/tool dependencies.
3. Confirm no third-party source was introduced into `include/` or `src/`
   (CI `supply-chain` job greps for foreign copyright/SPDX headers).
4. Archive `LICENSE` + `NOTICE.md` + SBOM with the GitHub Release.

---
*© 2026 norxs Technology LLC.*
