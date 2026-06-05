/**
 * =====================================================================================
 * @file        IamSecurityController.cpp
 * @brief       RBAC enforcement implementation. Policy lookups are read-only after
 *              Init() to allow lock-free hot-path authorisation. Audit entries are
 *              appended via a lock-free SPSC ring buffer to a static log array.
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#include "IamSecurityController.hpp"
#include <cstring>
#include <time.h>

namespace norxs {
namespace soa {

IamSecurityController::IamSecurityController() noexcept {
    roles_.fill(Role{});
    policies_.fill(PolicyEntry{});
    auditLog_.fill(AuditEntry{});
}

VoidResult IamSecurityController::Init() noexcept {
    bool expected = false;
    if (!initialised_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kAlreadyRegistered);
    }
    roleCount_   = 0U;
    policyCount_ = 0U;
    auditHead_.store(0U, std::memory_order_release);
    auditTail_.store(0U, std::memory_order_release);
    return VoidResult::Ok();
}

VoidResult IamSecurityController::RegisterRole(Role const& role) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (roleCount_ >= kMaxRoles) {
        return VoidResult::Err(ErrorCode::kRegistryFull);
    }
    for (std::uint8_t i = 0U; i < roleCount_; ++i) {
        if (roles_[i].roleId == role.roleId) {
            return VoidResult::Err(ErrorCode::kAlreadyRegistered);
        }
    }
    roles_[roleCount_] = role;
    ++roleCount_;
    return VoidResult::Ok();
}

VoidResult IamSecurityController::AddPolicy(PolicyEntry const& entry) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (policyCount_ >= kMaxPolicies) {
        return VoidResult::Err(ErrorCode::kRegistryFull);
    }
    policies_[policyCount_] = entry;
    ++policyCount_;
    return VoidResult::Ok();
}

std::uint64_t IamSecurityController::GetMonotonicNs() noexcept {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL) +
            static_cast<std::uint64_t>(ts.tv_nsec);
}

void IamSecurityController::AppendAuditEntry(std::uint32_t principalId,
                                              std::uint16_t serviceId,
                                              std::uint16_t methodId,
                                              AccessAction  action,
                                              AuditResult   result) noexcept {
    std::uint8_t head = auditHead_.load(std::memory_order_relaxed);
    std::uint8_t const nextHead =
        static_cast<std::uint8_t>((head + 1U) % kMaxAuditLogEntries);

    AuditEntry& slot   = auditLog_[head];
    slot.principalId   = principalId;
    slot.serviceId     = serviceId;
    slot.methodId      = methodId;
    slot.requested     = action;
    slot.result        = result;
    slot.timestampNs   = GetMonotonicNs();

    auditHead_.store(nextHead, std::memory_order_release);
}

VoidResult IamSecurityController::Authorise(std::uint32_t principalId,
                                             std::uint16_t serviceId,
                                             std::uint16_t methodId,
                                             AccessAction  action) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }

    for (std::uint8_t i = 0U; i < policyCount_; ++i) {
        PolicyEntry const& p = policies_[i];

        bool const principalMatch = (p.principalId == principalId);
        bool const serviceMatch   = (p.serviceId   == serviceId);
        bool const methodMatch    = (p.methodId    == 0xFFFFU) ||
                                    (p.methodId    == methodId);

        if (principalMatch && serviceMatch && methodMatch) {
            if (HasAction(p.allowed, action)) {
                AppendAuditEntry(principalId, serviceId, methodId,
                                 action, AuditResult::kGranted);
                return VoidResult::Ok();
            }
            AppendAuditEntry(principalId, serviceId, methodId,
                             action, AuditResult::kDenied);
            return VoidResult::Err(ErrorCode::kUnauthorized);
        }
    }

    // Default deny
    AppendAuditEntry(principalId, serviceId, methodId,
                     action, AuditResult::kDenied);
    return VoidResult::Err(ErrorCode::kUnauthorized);
}

std::uint8_t IamSecurityController::DrainAuditLog(AuditEntry*  buffer,
                                                    std::uint8_t bufferSize) noexcept {
    if (buffer == nullptr) {
        return 0U;
    }

    std::uint8_t copied = 0U;
    while (copied < bufferSize) {
        std::uint8_t tail = auditTail_.load(std::memory_order_relaxed);
        std::uint8_t head = auditHead_.load(std::memory_order_acquire);
        if (tail == head) {
            break;
        }
        buffer[copied] = auditLog_[tail];
        ++copied;
        std::uint8_t const nextTail =
            static_cast<std::uint8_t>((tail + 1U) % kMaxAuditLogEntries);
        auditTail_.store(nextTail, std::memory_order_release);
    }

    return copied;
}

} // namespace soa
} // namespace norxs
