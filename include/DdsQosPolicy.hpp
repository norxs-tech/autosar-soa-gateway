/**
 * =====================================================================================
 * @file        DdsQosPolicy.hpp
 * @brief       DDS Quality of Service (QoS) policy definitions and enforcement for
 *              the SOA Gateway DDS communication layer.  Implements the AUTOSAR
 *              Adaptive DDS binding QoS subset required for ADAS control-loop
 *              determinism: Reliability, Deadline (Latency Budget), Durability,
 *              and History.
 *
 *              Policy enforcement model:
 *                - Each DDS topic has one associated DdsQosProfile (stack-allocated).
 *                - The DdsQosEnforcer validates incoming and outgoing DDS samples
 *                  against the active profile before they enter the SOA pipeline.
 *                - Deadline monitoring uses the same CLOCK_MONOTONIC as WdgM to
 *                  ensure consistent time domain.
 *                - History depth is enforced by dropping oldest samples (ring
 *                  semantics) within the static SoaEvent queue — no heap required.
 *
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, DDS-XTYPES 1.2, OMG DDS 1.4, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#ifndef NORXS_SOA_DDSQOSPOLICY_HPP
#define NORXS_SOA_DDSQOSPOLICY_HPP

#include "SoaServiceManager.hpp"
#include <cstdint>
#include <array>
#include <atomic>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// QoS constants
// ---------------------------------------------------------------------------

static constexpr std::uint8_t  kMaxQosProfiles     = 16U;
static constexpr std::uint32_t kInvalidTopicId     = 0xFFFFFFFFU;
static constexpr std::uint32_t kDeadlineInfiniteMs = 0xFFFFFFFFU;

// ---------------------------------------------------------------------------
// Reliability policy
// ---------------------------------------------------------------------------

enum class ReliabilityKind : std::uint8_t {
    kBestEffort  = 0U, ///< Fire-and-forget; no retransmission (sensor telemetry)
    kReliable    = 1U  ///< Guaranteed delivery with retransmission (control commands)
};

// ---------------------------------------------------------------------------
// Durability policy
// ---------------------------------------------------------------------------

enum class DurabilityKind : std::uint8_t {
    kVolatile        = 0U, ///< No persistence; late-joining readers miss past samples
    kTransientLocal  = 1U  ///< Publisher caches History depth samples for late joiners
};

// ---------------------------------------------------------------------------
// History policy
// ---------------------------------------------------------------------------

enum class HistoryKind : std::uint8_t {
    kKeepLast = 0U, ///< Retain only the N most recent samples
    kKeepAll  = 1U  ///< Retain all samples until acknowledged (Reliable only)
};

// ---------------------------------------------------------------------------
// DDS QoS Profile (one per topic, fully stack-allocated)
// ---------------------------------------------------------------------------

struct DdsQosProfile {
    std::uint32_t    topicId          { kInvalidTopicId };
    char             topicName[32]    {};

    // Reliability
    ReliabilityKind  reliability      { ReliabilityKind::kBestEffort };

    // Deadline: maximum inter-sample interval the reader/writer commits to.
    // Expressed in milliseconds. Violation triggers kTimeout error to the
    // service manager for escalation to the safety degradation matrix.
    std::uint32_t    deadlineMs       { kDeadlineInfiniteMs };

    // Latency budget: advisory maximum end-to-end latency for the DDS layer.
    // Used for monitoring and diagnostics; does not cause packet drops.
    std::uint32_t    latencyBudgetMs  { 1U };  ///< 1ms default (ADAS control loop)

    // Durability
    DurabilityKind   durability       { DurabilityKind::kVolatile };

    // History
    HistoryKind      history          { HistoryKind::kKeepLast };
    std::uint8_t     historyDepth     { 1U }; ///< Relevant only for kKeepLast
};

// ---------------------------------------------------------------------------
// Per-topic deadline tracking state
// ---------------------------------------------------------------------------

struct DeadlineState {
    std::uint32_t          topicId       { kInvalidTopicId };
    std::atomic<uint64_t>  lastSampleMs  { 0U };  ///< Timestamp of last received sample
    std::atomic<uint32_t>  violationCount{ 0U };   ///< Cumulative deadline misses
    bool                   active        { false };

    DeadlineState() noexcept = default;
    DeadlineState(DeadlineState const&)            = delete;
    DeadlineState& operator=(DeadlineState const&) = delete;
    DeadlineState(DeadlineState&&)                 = delete;
    DeadlineState& operator=(DeadlineState&&)      = delete;
};

// ---------------------------------------------------------------------------
// DdsQosEnforcer
// ---------------------------------------------------------------------------

class DdsQosEnforcer final {
public:
    DdsQosEnforcer() noexcept = default;
    ~DdsQosEnforcer() noexcept = default;

    DdsQosEnforcer(DdsQosEnforcer const&)            = delete;
    DdsQosEnforcer& operator=(DdsQosEnforcer const&) = delete;
    DdsQosEnforcer(DdsQosEnforcer&&)                 = delete;
    DdsQosEnforcer& operator=(DdsQosEnforcer&&)      = delete;

    /**
     * @brief  Initialise the enforcer; must be called before any other method.
     */
    VoidResult Init() noexcept;

    /**
     * @brief  Register a QoS profile for a topic.
     * @return VoidResult::Ok() or Err(kRegistryFull / kAlreadyRegistered).
     */
    VoidResult RegisterProfile(DdsQosProfile const& profile) noexcept;

    /**
     * @brief  Validate an incoming DDS sample against its topic's QoS profile.
     *         Checks:
     *           1. Topic is registered (kServiceNotFound if not).
     *           2. Sample is within the reliability contract.
     *           3. History: if depth exceeded and policy is kKeepLast, returns
     *              Err(kQueueFull) — caller must drop the oldest sample first.
     *         Updates the deadline state's lastSampleMs on success.
     *
     * @param  topicId    DDS topic identifier.
     * @param  sampleLen  Serialised sample byte length.
     * @return VoidResult::Ok() if the sample may proceed into the SOA pipeline.
     */
    VoidResult ValidateSample(std::uint32_t topicId,
                               std::uint16_t sampleLen) noexcept;

    /**
     * @brief  Check all registered topics for deadline violations.
     *         Must be called periodically (e.g. every 1ms).
     *         On violation, increments violationCount and returns the first
     *         violating topicId via outTopicId.
     *
     * @param[out] outTopicId   First topic ID found in violation (if any).
     * @return VoidResult::Ok() if all deadlines met; Err(kTimeout) on first miss.
     */
    VoidResult CheckDeadlines(std::uint32_t& outTopicId) noexcept;

    /**
     * @brief  Return the QoS profile for a given topic.
     */
    Result<DdsQosProfile const*> GetProfile(std::uint32_t topicId) const noexcept;

    /**
     * @brief  Return cumulative deadline violation count for a topic.
     */
    Result<std::uint32_t> GetViolationCount(std::uint32_t topicId) const noexcept;

private:
    bool FindProfileIndex(std::uint32_t topicId, std::uint8_t& idx) const noexcept;
    bool FindDeadlineIndex(std::uint32_t topicId, std::uint8_t& idx) const noexcept;
    static std::uint64_t GetMonotonicMs() noexcept;

    std::array<DdsQosProfile, kMaxQosProfiles>  profiles_{};
    std::array<DeadlineState, kMaxQosProfiles>  deadlines_{};
    std::uint8_t                                profileCount_{ 0U };
    std::atomic<bool>                           initialised_{ false };
};

} // namespace soa
} // namespace norxs

#endif // NORXS_SOA_DDSQOSPOLICY_HPP
