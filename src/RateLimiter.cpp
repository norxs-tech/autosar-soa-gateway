/**
 * =====================================================================================
 * @file        RateLimiter.cpp
 * @brief       Token Bucket rate limiter implementation. Lock-free Admit() uses
 *              compare-exchange on per-bucket atomic token counters. RefillAll()
 *              computes elapsed milliseconds via CLOCK_MONOTONIC and credits tokens
 *              proportionally, clamping at kBurstCapacity. EvictIdle() reclaims
 *              stale source slots to prevent table exhaustion from address spoofing.
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#include "RateLimiter.hpp"
#include <time.h>
#include <cstring>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// GetMonotonicMs (POSIX)
// ---------------------------------------------------------------------------

std::uint64_t RateLimiter::GetMonotonicMs() noexcept {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL) +
            static_cast<std::uint64_t>(static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ULL);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

VoidResult RateLimiter::Init() noexcept {
    bool expected = false;
    if (!initialised_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kAlreadyRegistered);
    }

    // Explicitly reset all atomic fields (cannot use fill() on non-copyable types)
    std::uint64_t const nowMs = GetMonotonicMs();
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        buckets_[i].sourceId     = kInvalidSourceId;
        buckets_[i].tokens.store(kBurstCapacity,  std::memory_order_relaxed);
        buckets_[i].lastRefillMs.store(nowMs,       std::memory_order_relaxed);
        buckets_[i].lastSeenMs.store(nowMs,         std::memory_order_relaxed);
        buckets_[i].dropCount.store(0U,             std::memory_order_relaxed);
        buckets_[i].passCount.store(0U,             std::memory_order_relaxed);
        buckets_[i].active.store(false,             std::memory_order_relaxed);
    }

    std::atomic_thread_fence(std::memory_order_release);
    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// FindOrAllocate (private, not lock-free – called only from Admit single path)
// ---------------------------------------------------------------------------

std::uint8_t RateLimiter::FindOrAllocate(std::uint32_t sourceId) noexcept {
    // First pass: find existing active entry
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        if (buckets_[i].active.load(std::memory_order_acquire) &&
            (buckets_[i].sourceId == sourceId)) {
            return i;
        }
    }
    // Second pass: allocate a free slot
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        bool expected = false;
        if (buckets_[i].active.compare_exchange_strong(expected, true,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            // Slot acquired — initialise it
            std::uint64_t const nowMs = GetMonotonicMs();
            buckets_[i].sourceId = sourceId;
            buckets_[i].tokens.store(kBurstCapacity, std::memory_order_relaxed);
            buckets_[i].lastRefillMs.store(nowMs,    std::memory_order_relaxed);
            buckets_[i].lastSeenMs.store(nowMs,      std::memory_order_relaxed);
            buckets_[i].dropCount.store(0U,          std::memory_order_relaxed);
            buckets_[i].passCount.store(0U,          std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            return i;
        }
    }
    return kMaxTrackedSources; // Table full
}

// ---------------------------------------------------------------------------
// RefillBucket (private)
// ---------------------------------------------------------------------------

void RateLimiter::RefillBucket(TokenBucket& bucket, std::uint64_t nowMs) noexcept {
    std::uint64_t const lastMs =
        bucket.lastRefillMs.load(std::memory_order_relaxed);

    if (nowMs <= lastMs) {
        return; // Clock not advanced or wrapped
    }

    std::uint64_t const elapsedMs = nowMs - lastMs;
    // Clamp to avoid overflow on very large gaps
    std::uint64_t const clampedMs =
        (elapsedMs > 1000U) ? 1000U : elapsedMs;

    std::int32_t const newTokens =
        static_cast<std::int32_t>(clampedMs) * kRefillRatePerMs;

    // CAS loop to add tokens, clamped at kBurstCapacity
    std::int32_t current = bucket.tokens.load(std::memory_order_relaxed);
    while (true) {
        std::int32_t const updated =
            (current + newTokens > kBurstCapacity) ? kBurstCapacity
                                                    : (current + newTokens);
        if (bucket.tokens.compare_exchange_weak(current, updated,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
            break;
        }
        // current reloaded by CAS failure – retry
    }

    bucket.lastRefillMs.store(nowMs, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Admit  (lock-free fast path)
// ---------------------------------------------------------------------------

VoidResult RateLimiter::Admit(std::uint32_t sourceId) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }

    std::uint8_t const idx = FindOrAllocate(sourceId);
    if (idx >= kMaxTrackedSources) {
        // Table exhausted — fail-secure: deny unknown sources
        return VoidResult::Err(ErrorCode::kRegistryFull);
    }

    TokenBucket& bucket = buckets_[idx];

    // Update last-seen timestamp
    bucket.lastSeenMs.store(GetMonotonicMs(), std::memory_order_relaxed);

    // Lock-free token deduction via CAS
    std::int32_t current = bucket.tokens.load(std::memory_order_relaxed);
    while (true) {
        if (current <= 0) {
            // Bucket empty — drop
            bucket.dropCount.fetch_add(1U, std::memory_order_relaxed);
            return VoidResult::Err(ErrorCode::kQueueFull);
        }
        if (bucket.tokens.compare_exchange_weak(current, current - 1,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {
            bucket.passCount.fetch_add(1U, std::memory_order_relaxed);
            return VoidResult::Ok();
        }
        // current reloaded by CAS failure — retry
    }
}

// ---------------------------------------------------------------------------
// RefillAll
// ---------------------------------------------------------------------------

void RateLimiter::RefillAll() noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return;
    }

    std::uint64_t const nowMs = GetMonotonicMs();
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        if (buckets_[i].active.load(std::memory_order_acquire)) {
            RefillBucket(buckets_[i], nowMs);
        }
    }
}

// ---------------------------------------------------------------------------
// EvictIdle
// ---------------------------------------------------------------------------

void RateLimiter::EvictIdle() noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return;
    }

    std::uint64_t const nowMs = GetMonotonicMs();
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        if (!buckets_[i].active.load(std::memory_order_acquire)) {
            continue;
        }

        std::uint64_t const lastSeen =
            buckets_[i].lastSeenMs.load(std::memory_order_relaxed);

        if (nowMs >= lastSeen &&
            (nowMs - lastSeen) > kSourceEvictAfterMs) {
            // Mark slot as free — future FindOrAllocate() can reclaim it
            buckets_[i].sourceId = kInvalidSourceId;
            buckets_[i].tokens.store(kBurstCapacity,  std::memory_order_relaxed);
            buckets_[i].dropCount.store(0U,            std::memory_order_relaxed);
            buckets_[i].passCount.store(0U,            std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            // Release active last so no window where sourceId is invalid but active
            buckets_[i].active.store(false, std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------
// GetStats
// ---------------------------------------------------------------------------

bool RateLimiter::GetStats(std::uint32_t sourceId,
                             RateLimiterStats& stats) const noexcept {
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        if (buckets_[i].active.load(std::memory_order_acquire) &&
            (buckets_[i].sourceId == sourceId)) {
            stats.sourceId  = sourceId;
            stats.tokens    = buckets_[i].tokens.load(std::memory_order_relaxed);
            stats.dropCount = buckets_[i].dropCount.load(std::memory_order_relaxed);
            stats.passCount = buckets_[i].passCount.load(std::memory_order_relaxed);
            stats.active    = true;
            return true;
        }
    }
    stats = RateLimiterStats{};
    return false;
}

// ---------------------------------------------------------------------------
// ActiveSourceCount
// ---------------------------------------------------------------------------

std::uint8_t RateLimiter::ActiveSourceCount() const noexcept {
    std::uint8_t count = 0U;
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        if (buckets_[i].active.load(std::memory_order_relaxed)) {
            ++count;
        }
    }
    return count;
}

} // namespace soa
} // namespace norxs
