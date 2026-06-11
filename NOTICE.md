# NOTICE — Third-Party Software

This file satisfies the open source notice obligations of the norxs Technology
LLC open source compliance program (OpenChain **ISO/IEC 5230:2020** conformant).

---

## Production Code

The production library (`include/`, `src/`) contains **no third-party source
code** and has **no third-party runtime dependencies**. It links only against
the C++14 standard library and platform headers (POSIX / QNX SDP 8.0 /
arm-none-eabi newlib, depending on the selected toolchain).

A machine-readable Software Bill of Materials is provided in SPDX 2.3 format:

```
sbom/norxs-soa-gateway.spdx.json
```

## Development / Test-Only Dependencies

The following components are used **exclusively at development time** and are
**not distributed** with, nor linked into, the production library:

| Component | Version | License | Use |
|-----------|---------|---------|-----|
| GoogleTest (googletest) | ≥ 1.12 | BSD-3-Clause | Unit test framework (`tests/`, `BUILD_TESTS=ON` only) |
| gcovr | ≥ 6.0 | BSD-3-Clause | Coverage report generation (CI only) |
| cppcheck | ≥ 2.7 | GPL-3.0-or-later | Static analysis tool (CI only; tool use, no code linkage) |
| lcov / genhtml | ≥ 1.14 | GPL-2.0-or-later | Optional local coverage HTML (tool use, no code linkage) |
| CMake | ≥ 3.16 | BSD-3-Clause | Build system (tool use) |

GoogleTest — Copyright 2008 Google Inc. Licensed under the BSD-3-Clause
license. The full license text is available at
https://github.com/google/googletest/blob/main/LICENSE

## License of This Repository

This repository itself is licensed under the
**norxs Reference Implementation License v1.0** (see `LICENSE`).
It is a proprietary source-available license, **not** an OSI-approved open
source license. Open-source license inquiries: `contact@norxs.com`.

---

*© 2026 norxs Technology LLC. All rights reserved.*
