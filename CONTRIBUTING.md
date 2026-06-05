# Contributing to norxs SOA Gateway

Thank you for your interest in contributing to the norxs SOA Gateway reference
implementation. Please read this guide carefully before submitting any changes.

---

## License Agreement

By submitting a pull request, you agree that your contribution is made under
the **norxs Reference Implementation License v1.0** (see `LICENSE`) and that
you grant norxs Technology LLC a perpetual, irrevocable, royalty-free license
to use, modify, and redistribute your contribution as part of this project.

If your employer holds rights to the code you produce, you must ensure you
have authorization to contribute before submitting.

---

## Coding Standards (Mandatory)

All contributions to `include/` and `src/` **must** comply with the following.
Pull requests failing any of these checks will be rejected without review.

### AUTOSAR C++14 / MISRA C++:2008

| Rule | Requirement |
|------|-------------|
| No dynamic allocation | `new`, `malloc`, `calloc`, `realloc` forbidden post-`Init()`. Use `std::array` or pre-allocated pools. |
| No exceptions | `try`, `catch`, `throw` are forbidden. Use `Result<T>` / `VoidResult`. |
| No RTTI | `dynamic_cast`, `typeid` are forbidden. |
| No `std::function` | Use plain function pointers (`using Fn = void(*)(...)`) to avoid heap allocation. |
| No owning smart pointers | `std::shared_ptr`, `std::unique_ptr` creation forbidden. Non-owning raw pointers are allowed with documented lifetime. |
| All functions `noexcept` | Every method that cannot throw must be marked `noexcept`. |
| Explicit casts only | Use `static_cast`, `reinterpret_cast` with justification comment. Never C-style casts. |

### Naming Convention

```
Classes / Structs   : PascalCase           (SafetyArbitrator)
Methods             : PascalCase           (IngestFault)
Member variables    : trailing underscore  (initialised_)
Constants           : k-prefix + PascalCase (kMaxServices)
Enum values         : k-prefix + PascalCase (ErrorCode::kTimeout)
Local variables     : camelCase            (sensorHealth)
```

### Error Handling

All non-void functions that can fail **must** return `VoidResult` or `Result<T>`.
Never return `bool`. Never silently swallow errors.

```cpp
// CORRECT
VoidResult MyClass::DoSomething() noexcept {
    if (ptr_ == nullptr) {
        return VoidResult::Err(ErrorCode::kNullPointer);
    }
    return VoidResult::Ok();
}

// FORBIDDEN
bool MyClass::DoSomething() {  // no bool return, no exception
    ...
}
```

### Concurrency

- All state shared between threads must be `std::atomic<T>`.
- Use `std::atomic_thread_fence(std::memory_order_seq_cst)` before advancing
  any ring-buffer head/tail pointer visible to another core.
- `std::mutex`, `std::lock_guard`, `std::condition_variable` are **forbidden**
  on any data path that executes within 1ms of real-time deadlines.

### Doxygen Header (Required in Every File)

```cpp
/**
 * =====================================================================================
 * @file        [filename]
 * @brief       [Comprehensive one-paragraph description]
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, [applicable standards]
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */
```

---

## Testing Requirements

Every code change must be accompanied by unit tests in `tests/`.

- Framework: **GoogleTest** (gtest)
- Coverage target: **> 90% MC/DC** (Modified Condition/Decision Coverage)
  for all decision points in ASIL-D modules
  (`SafetyArbitrator`, `IpcBridge`, `IamSecurityController`)
- All `static_assert` expressions on struct sizes and alignment must be present
  for any new IPC-facing struct
- Fault injection tests are required for any new safety-critical state machine

Run tests locally before submitting:

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./soa_gateway_tests
./safety_arbitrator_tests
make coverage
```

Coverage report is generated at `build/coverage_html/index.html`.

---

## Pull Request Process

1. **Fork** the repository and create a feature branch:
   ```
   git checkout -b feature/your-module-name
   ```

2. **Write code** following all coding standards above.

3. **Write tests** with > 90% MC/DC coverage.

4. **Run static analysis** and fix all issues:
   ```bash
   make analyze
   # Zero errors allowed in cppcheck_report.xml
   ```

5. **Update `CHANGELOG.md`** under `[Unreleased]` with a description of your change.

6. **Submit a pull request** with:
   - A clear description of what the change does and why
   - Reference to any relevant standard clause (e.g., "ISO 26262 Part 6 §9.4")
   - Confirmation that all tests pass and coverage target is met

7. Pull requests are reviewed by a norxs engineer. Feedback will be provided
   within 10 business days.

---

## Issue Reporting

Use the GitHub Issue templates for:
- **Bug reports**: include reproduction steps, expected vs actual behavior,
  and the affected module
- **Feature requests**: include the motivating standard clause or use case

Security vulnerabilities must **not** be reported via public GitHub Issues.
Send a confidential report to the contact listed on https://norxs.com.

---

## Questions

For engineering questions, open a GitHub Discussion. For commercial licensing
or safety certification inquiries, contact norxs Technology LLC directly at
https://norxs.com.

---

*norxs Technology LLC — Safety Engineering, Built from the Ground Up.*
