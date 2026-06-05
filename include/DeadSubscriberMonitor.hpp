/**
 * =====================================================================================
 * @file        DeadSubscriberMonitor.hpp
 * @brief       Dead subscriber detection and connection pool management for the
 *              SOA Gateway.  Tracks the liveness of all registered SOME/IP and DDS
 *              subscribers via a periodic heartbeat TTL mechanism and proactively
 *              reclaims stale subscriber slots in SoaServiceManager to prevent
 *              socket/connection pool exhaustion on the Cortex-A53.
 *
 *              Liveness model:
 *                - Every active subscriber must call RecordHeartbeat() at intervals
 *                  no greater than kHeartbeatIntervalMs (default 500ms).
 *                - The monitor task (called at kMonitorPeriodMs, default 200ms) scans
 *                  the liveness table and marks as DEAD any subscriber whose last
 *                  heartbeat exceeds kDeadTimeoutMs (default 2000ms).
 *                - On detection, an on-death callback (DeadCallback) is invoked,
 *                  which the SoaServiceManager uses to remove the subscriber entry
 *                  and release the socket file descriptor.
 *                - Zero dynamic allocation: liveness table is a static std::array.
 *                - Lock-free: heartbeat updates use std::atomic<uint64_t> timestamps.
 *
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#ifndef NORXS_SOA_DEADSUBSCRIBERMONITOR_HPP
#define NORXS_SOA_DEADSUBSCRIBERMONITOR_HPP

#include "SoaServiceManager.hpp"
#include <cstdint>
#include <array>
#include <atomic>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// Monitor constants
// ---------------------------------------------------------------------------

static constexpr std::uint8_t  kMaxMonitoredSubs   = 32U;    ///< Max tracked subs
static constexpr std::uint32_t kHeartbeatIntervalMs = 500U;  ///< Subscriber duty
static constexpr std::uint32_t kDeadTimeoutMs       = 2000U; ///< Eviction threshold
static constexpr std::uint32_t kMonitorPeriodMs     = 200U;  ///< Monitor scan period

// ---------------------------------------------------------------------------
// Subscriber liveness state
// ---------------------------------------------------------------------------

enum class SubscriberHealth : std::uint8_t {
    kAlive    = 0U,
    kWarning  = 1U, ///< One missed heartbeat interval
    kDead     = 2U  ///< Exceeded kDeadTimeoutMs — evict
};

// ---------------------------------------------------------------------------
// On-death callback (plain function pointer — no heap)
// Parameters: serviceId, eventId of the dead subscriber
// ---------------------------------------------------------------------------

using DeadCallback = void (*)(std::uint16_t serviceId,
                               std::uint16_t eventId);

// ---------------------------------------------------------------------------
// Liveness entry
// ---------------------------------------------------------------------------

struct LivenessEntry {
    std::uint16_t          serviceId   { kInvalidServiceId };
    std::uint16_t          eventId     { 0U };
    std::atomic<uint64_t>  lastBeatMs  { 0U };
    std::atomic<uint8_t>   health      { static_cast<std::uint8_t>(
                                            SubscriberHealth::kAlive) };
    DeadCallback           onDead      { nullptr };
    std::atomic<bool>      active      { false };

    LivenessEntry() noexcept = default;
    LivenessEntry(LivenessEntry const&)            = delete;
    LivenessEntry& operator=(LivenessEntry const&) = delete;
    LivenessEntry(LivenessEntry&&)                 = delete;
    LivenessEntry& operator=(LivenessEntry&&)      = delete;
};

// ---------------------------------------------------------------------------
// Monitor statistics snapshot (copyable)
// ---------------------------------------------------------------------------

struct MonitorStats {
    std::uint8_t totalActive   { 0U };
    std::uint8_t totalAlive    { 0U };
    std::uint8_t totalWarning  { 0U };
    std::uint8_t totalDead     { 0U };
    std::uint32_t evictedTotal { 0U };
};

// ---------------------------------------------------------------------------
// DeadSubscriberMonitor
// ---------------------------------------------------------------------------

class DeadSubscriberMonitor final {
public:
    DeadSubscriberMonitor() noexcept = default;
    ~DeadSubscriberMonitor() noexcept = default;

    DeadSubscriberMonitor(DeadSubscriberMonitor const&)            = delete;
    DeadSubscriberMonitor& operator=(DeadSubscriberMonitor const&) = delete;
    DeadSubscriberMonitor(DeadSubscriberMonitor&&)                 = delete;
    DeadSubscriberMonitor& operator=(DeadSubscriberMonitor&&)      = delete;

    /**
     * @brief  Initialise the monitor; must be called before any other method.
     */
    VoidResult Init();

    /**
     * @brief  Register a new subscriber for liveness monitoring.
     * @param  serviceId  Service to which the subscriber is attached.
     * @param  eventId    Event group ID within the service.
     * @param  onDead     Callback invoked when the subscriber is declared dead.
     *                    Must be a non-null static function pointer.
     * @return VoidResult::Ok() or Err(kRegistryFull / kAlreadyRegistered).
     */
    VoidResult Register(std::uint16_t serviceId,
                         std::uint16_t eventId,
                         DeadCallback  onDead);

    /**
     * @brief  Record a heartbeat for the given subscriber.
     *         Called by the subscriber itself or by the network receive path
     *         when a SOME/IP subscription-ack or DDS participant announcement
     *         is received from the corresponding client.
     *         Lock-free: updates a single atomic<uint64_t> timestamp.
     */
    VoidResult RecordHeartbeat(std::uint16_t serviceId,
                                std::uint16_t eventId);

    /**
     * @brief  Scan all monitored subscribers for liveness violations.
     *         Must be called from the periodic maintenance task at kMonitorPeriodMs.
     *         On detecting a dead subscriber:
     *           1. Sets health to kDead.
     *           2. Invokes onDead() callback (which removes from SoaServiceManager).
     *           3. Frees the liveness slot for re-use.
     *         Returns the count of subscribers evicted in this scan cycle.
     */
    std::uint8_t ScanAndEvict();

    /**
     * @brief  Manually deregister a subscriber (e.g., on graceful disconnect).
     */
    VoidResult Deregister(std::uint16_t serviceId,
                           std::uint16_t eventId);

    /**
     * @brief  Return a diagnostic snapshot of the current monitor state.
     */
    MonitorStats GetStats() const noexcept;

private:
    bool FindIndex(std::uint16_t serviceId,
                   std::uint16_t eventId,
                   std::uint8_t& idx) const noexcept;

    static std::uint64_t GetMonotonicMs();

    std::array<LivenessEntry, kMaxMonitoredSubs> entries_{};
    std::atomic<std::uint32_t>                   evictedTotal_{ 0U };
    std::atomic<bool>                            initialised_{ false };
};

} // namespace soa
} // namespace norxs

#endif // NORXS_SOA_DEADSUBSCRIBERMONITOR_HPP
