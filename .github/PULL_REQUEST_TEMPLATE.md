## Summary
<!-- What does this PR do? One paragraph. -->

## Motivation
<!-- Why is this change needed? Reference the relevant Issue # or standard clause. -->
Closes #

## Module(s) Changed
- [ ] SoaServiceManager
- [ ] NetworkAdapter
- [ ] IamSecurityController
- [ ] IpcBridge
- [ ] HseAdapter
- [ ] RateLimiter
- [ ] DdsQosPolicy
- [ ] DeadSubscriberMonitor
- [ ] SafetyArbitrator
- [ ] Build / CMake
- [ ] Documentation / Tests

## AUTOSAR C++14 Compliance Checklist
<!-- Every item must be checked before a PR will be reviewed. -->

- [ ] No `new`, `malloc`, `calloc`, or `realloc` after `Init()` phase
- [ ] No `try`, `catch`, or `throw` — all errors use `Result<T>` / `VoidResult`
- [ ] No `std::mutex`, `lock_guard`, or `condition_variable` on data paths
- [ ] No `std::vector`, `std::list`, `std::map` or other dynamic containers
- [ ] No `std::shared_ptr` / `std::unique_ptr` ownership (non-owning raw ptrs OK)
- [ ] All new public methods are `noexcept`
- [ ] All new public methods are documented with Doxygen `@brief` / `@param` / `@return`
- [ ] Doxygen corporate header present in every new file
- [ ] Include guards (`#ifndef NORXS_SOA_`) present in every new header

## Safety & Security Checklist
- [ ] New IPC-facing structs have `static_assert` on their `sizeof`
- [ ] New ring-buffer operations have `atomic_thread_fence(seq_cst)` before head advance
- [ ] New RBAC policy paths default-deny on no match
- [ ] `CHANGELOG.md` updated under `[Unreleased]`

## Testing
- [ ] Unit tests added / updated in `tests/`
- [ ] MC/DC branch coverage ≥ 90% on changed modules (attach `lcov --summary` output)
- [ ] Fault injection test added for any new state machine
- [ ] `make analyze` (cppcheck) passes with zero errors

## MC/DC Coverage Summary
```
# Paste output of: lcov --summary build/lcov.info --rc lcov_branch_coverage=1
```

## Test Results
```
# Paste: ./soa_gateway_tests && ./safety_arbitrator_tests
```
