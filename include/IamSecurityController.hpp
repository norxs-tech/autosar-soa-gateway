/**
 * =====================================================================================
 * @file        IamSecurityController.hpp
 * @brief       Identity & Access Management (IAM) firewall implementing Role-Based
 *              Access Control (RBAC) for SOA microservice calls. Enforces UN R155
 *              cybersecurity requirements by blocking unauthorised service interactions
 *              before they reach the IPC bridge. Zero dynamic allocation; policy
 *              table is a compile-time-sized std::array.
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#ifndef NORXS_SOA_IAMSECURITYCONTROLLER_HPP
#define NORXS_SOA_IAMSECURITYCONTROLLER_HPP

#include "SoaServiceManager.hpp"
#include <cstdint>
#include <array>
#include <atomic>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// RBAC constants
// ---------------------------------------------------------------------------

static constexpr std::uint8_t  kMaxPolicies        = 64U;
static constexpr std::uint8_t  kMaxRoles           = 16U;
static constexpr std::uint8_t  kMaxAuditLogEntries  = 128U;
static constexpr std::uint32_t kInvalidPrincipal   = 0xFFFFFFFFU;

// ---------------------------------------------------------------------------
// Access action bitmask
// ---------------------------------------------------------------------------

enum class AccessAction : std::uint8_t {
    kNone      = 0x00U,
    kRead      = 0x01U,
    kWrite     = 0x02U,
    kExecute   = 0x04U,
    kAdmin     = 0x08U,
    kAll       = 0x0FU
};

constexpr AccessAction operator|(AccessAction lhs, AccessAction rhs) noexcept {
    return static_cast<AccessAction>(
        static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr bool HasAction(AccessAction granted, AccessAction requested) noexcept {
    return (static_cast<std::uint8_t>(granted) &
            static_cast<std::uint8_t>(requested)) != 0U;
}

// ---------------------------------------------------------------------------
// Role descriptor
// ---------------------------------------------------------------------------

struct Role {
    std::uint8_t roleId   { 0U };
    char         name[24] {};
};

// ---------------------------------------------------------------------------
// RBAC Policy entry
// ---------------------------------------------------------------------------

struct PolicyEntry {
    std::uint32_t principalId { kInvalidPrincipal };
    std::uint16_t serviceId   { kInvalidServiceId };
    std::uint16_t methodId    { 0xFFFFU };
    std::uint8_t  roleId      { 0U };
    AccessAction  allowed     { AccessAction::kNone };
};

// ---------------------------------------------------------------------------
// Audit log entry (UN R155 §7.3.2 traceability)
// ---------------------------------------------------------------------------

enum class AuditResult : std::uint8_t {
    kGranted = 0U,
    kDenied  = 1U
};

struct AuditEntry {
    std::uint32_t  principalId { 0U };
    std::uint16_t  serviceId   { 0U };
    std::uint16_t  methodId    { 0U };
    AccessAction   requested   { AccessAction::kNone };
    AuditResult    result      { AuditResult::kDenied };
    std::uint64_t  timestampNs { 0U };
};

// ---------------------------------------------------------------------------
// IamSecurityController
// ---------------------------------------------------------------------------

class IamSecurityController final {
public:
    IamSecurityController() noexcept;
    ~IamSecurityController() noexcept = default;

    IamSecurityController(IamSecurityController const&)            = delete;
    IamSecurityController& operator=(IamSecurityController const&) = delete;
    IamSecurityController(IamSecurityController&&)                 = delete;
    IamSecurityController& operator=(IamSecurityController&&)      = delete;

    VoidResult Init() noexcept;
    VoidResult RegisterRole(Role const& role) noexcept;
    VoidResult AddPolicy(PolicyEntry const& entry) noexcept;
    VoidResult Authorise(std::uint32_t principalId,
                         std::uint16_t serviceId,
                         std::uint16_t methodId,
                         AccessAction  action) noexcept;
    std::uint8_t DrainAuditLog(AuditEntry*  buffer,
                                std::uint8_t bufferSize) noexcept;

private:
    void AppendAuditEntry(std::uint32_t principalId,
                          std::uint16_t serviceId,
                          std::uint16_t methodId,
                          AccessAction  action,
                          AuditResult   result) noexcept;
    static std::uint64_t GetMonotonicNs() noexcept;

    std::array<Role,         kMaxRoles>          roles_{};
    std::array<PolicyEntry,  kMaxPolicies>        policies_{};
    std::array<AuditEntry,   kMaxAuditLogEntries> auditLog_{};

    std::uint8_t              roleCount_    { 0U };
    std::uint8_t              policyCount_  { 0U };
    std::atomic<std::uint8_t> auditHead_    { 0U };
    std::atomic<std::uint8_t> auditTail_    { 0U };
    std::atomic<bool>         initialised_  { false };
};

} // namespace soa
} // namespace norxs

#endif // NORXS_SOA_IAMSECURITYCONTROLLER_HPP
