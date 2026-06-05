/**
 * =====================================================================================
 * @file        RateLimiter.hpp
 * @brief       Per-source Token Bucket rate limiter for the SOA Gateway network
 *              reception layer.  Enforces maximum packet rate per SOME/IP client ID
 *              at the NIC driver boundary, compliant with UN R155 §7.3.3 (anti-DDoS).
 *
 *              Algorithm: Token Bucket
 *                - Each tracked source has a token bucket of capacity kBurstCapacity.
 *                - Tokens refill at kRefillRatePerMs per millisecond of elapsed time.
 *                - Each admitted packet consumes one token.
 *                - A packet arriving at an empty bucket is DROPPED immediately without
 *                  forwarding to the deserialization stage, preventing CPU exhaustion
 *                  from malformed or flooding traffic.
 *
 *              Implementation constraints:
 *                - Zero dynamic allocation: source table is a static std::array.
 *                - Lock-free fast path: token deduction uses std::atomic<int32_t>
 *                  compare-exchange for thread safety between network IRQ context
 *                  and the SOA processing thread.
 *                - Time source: POSIX CLOCK_MONOTONIC via clock_gettime().
 *                - Capacity: tracks up to kMaxTrackedSources concurrent sources.
 *                  When the table is full, new (unseen) source IDs are DENIED by
 *                  default (fail-secure) to prevent table-exhaustion attacks.
 *
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#ifndef NORXS_SOA_RATELIMITER_HPP
#define NORXS_SOA_RATELIMITER_HPP

#include "SoaServiceManager.hpp"
#include <cstdint>
#include <array>
#include <atomic>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// Rate limiter policy constants
// ---------------------------------------------------------------------------

static constexpr std::uint8_t  kMaxTrackedSources  = 32U;  ///< Max concurrent sources
static constexpr std::int32_t  kBurstCapacity      = 20;   ///< Max burst tokens
static constexpr std::int32_t  kRefillRatePerMs    = 5;    ///< Tokens added per ms
static constexpr std::uint32_t kInvalidSourceId    = 0xFFFFFFFFU;
static constexpr std::uint64_t kSourceEvictAfterMs = 5000U; ///< Evict idle sources

// ---------------------------------------------------------------------------
// Per-source bucket state (lock-free via atomic token count)
// ---------------------------------------------------------------------------

struct TokenBucket {
    std::uint32_t         sourceId    { kInvalidSourceId };
    std::atomic<int32_t>  tokens      { kBurstCapacity };
    std::atomic<uint64_t> lastRefillMs{ 0U };
    std::atomic<uint64_t> lastSeenMs  { 0U };
    std::atomic<uint32_t> dropCount   { 0U };  ///< Diagnostic: packets dropped
    std::atomic<uint32_t> passCount   { 0U };  ///< Diagnostic: packets admitted
    std::atomic<bool>     active      { false };

    // Buckets are not copyable (atomics are not copyable)
    TokenBucket() noexcept = default;
    TokenBucket(TokenBucket const&)            = delete;
    TokenBucket& operator=(TokenBucket const&) = delete;
    TokenBucket(TokenBucket&&)                 = delete;
    TokenBucket& operator=(TokenBucket&&)      = delete;
};

// ---------------------------------------------------------------------------
// Diagnostic snapshot (copyable, for reporting)
// ---------------------------------------------------------------------------

struct RateLimiterStats {
    std::uint32_t sourceId   { kInvalidSourceId };
    std::int32_t  tokens     { 0 };
    std::uint32_t dropCount  { 0U };
    std::uint32_t passCount  { 0U };
    bool          active     { false };
};

// ---------------------------------------------------------------------------
// RateLimiter
// ---------------------------------------------------------------------------

class RateLimiter final {
public:
    RateLimiter() noexcept = default;
    ~RateLimiter() noexcept = default;

    RateLimiter(RateLimiter const&)            = delete;
    RateLimiter& operator=(RateLimiter const&) = delete;
    RateLimiter(RateLimiter&&)                 = delete;
    RateLimiter& operator=(RateLimiter&&)      = delete;

    /**
     * @brief  Initialise the rate limiter, zeroing all bucket state.
     */
    VoidResult Init() noexcept;

    /**
     * @brief  Evaluate whether a packet from sourceId should be admitted.
     *         Called on the fast path (NIC receive interrupt / driver thread).
     *         Lock-free: uses CAS on the per-bucket token counter.
     *
     * @param  sourceId  SOME/IP Client ID or source IP hash identifying the sender.
     * @return VoidResult::Ok()     → packet admitted, token deducted.
     *         VoidResult::Err(kQueueFull)      → bucket empty, packet MUST be dropped.
     *         VoidResult::Err(kRegistryFull)   → source table full, packet denied
     *                                            (fail-secure: unknown sources blocked
     *                                             when tracking capacity is exhausted).
     */
    VoidResult Admit(std::uint32_t sourceId) noexcept;

    /**
     * @brief  Refill tokens for all active buckets based on elapsed time.
     *         Must be called periodically (e.g. every 1ms from a timer task).
     *         Not on the critical fast path – uses relaxed atomic loads.
     */
    void RefillAll() noexcept;

    /**
     * @brief  Evict sources that have been idle for longer than kSourceEvictAfterMs.
     *         Should be called from the dead-subscriber / maintenance task.
     */
    void EvictIdle() noexcept;

    /**
     * @brief  Copy a diagnostic snapshot for a given source into stats.
     * @return true if the source is tracked, false otherwise.
     */
    bool GetStats(std::uint32_t sourceId, RateLimiterStats& stats) const noexcept;

    /**
     * @brief  Return total number of currently active source entries.
     */
    std::uint8_t ActiveSourceCount() const noexcept;

private:
    /**
     * @brief  Find the bucket index for sourceId, or allocate a new one.
     *         Returns kMaxTrackedSources if the table is full.
     */
    std::uint8_t FindOrAllocate(std::uint32_t sourceId) noexcept;

    /**
     * @brief  Refill a single bucket based on elapsed milliseconds.
     */
    void RefillBucket(TokenBucket& bucket, std::uint64_t nowMs) noexcept;

    static std::uint64_t GetMonotonicMs() noexcept;

    std::array<TokenBucket, kMaxTrackedSources> buckets_{};
    std::atomic<bool>                           initialised_{ false };
};

} // namespace soa
} // namespace norxs

#endif // NORXS_SOA_RATELIMITER_HPP
