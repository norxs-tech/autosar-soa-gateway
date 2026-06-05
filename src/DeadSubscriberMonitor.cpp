/**
 * =====================================================================================
 * @file        DeadSubscriberMonitor.cpp
 * @brief       Dead subscriber monitor implementation: liveness table management,
 *              lock-free heartbeat recording, periodic TTL-based eviction scan,
 *              and on-death callback dispatch.
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#include "DeadSubscriberMonitor.hpp"
#include <time.h>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// GetMonotonicMs
// ---------------------------------------------------------------------------

std::uint64_t DeadSubscriberMonitor::GetMonotonicMs() noexcept {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL) +
            static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ULL;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

VoidResult DeadSubscriberMonitor::Init() noexcept {
    bool expected = false;
    if (!initialised_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kAlreadyRegistered);
    }

    std::uint64_t const nowMs = GetMonotonicMs();
    for (std::uint8_t i = 0U; i < kMaxMonitoredSubs; ++i) {
        entries_[i].serviceId = kInvalidServiceId;
        entries_[i].eventId   = 0U;
        entries_[i].onDead    = nullptr;
        entries_[i].lastBeatMs.store(nowMs, std::memory_order_relaxed);
        entries_[i].health.store(static_cast<std::uint8_t>(SubscriberHealth::kAlive),
                                  std::memory_order_relaxed);
        entries_[i].active.store(false, std::memory_order_relaxed);
    }
    evictedTotal_.store(0U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// FindIndex
// ---------------------------------------------------------------------------

bool DeadSubscriberMonitor::FindIndex(std::uint16_t serviceId,
                                       std::uint16_t eventId,
                                       std::uint8_t& idx) const noexcept {
    for (std::uint8_t i = 0U; i < kMaxMonitoredSubs; ++i) {
        if (entries_[i].active.load(std::memory_order_acquire) &&
            (entries_[i].serviceId == serviceId) &&
            (entries_[i].eventId   == eventId)) {
            idx = i;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

VoidResult DeadSubscriberMonitor::Register(std::uint16_t serviceId,
                                            std::uint16_t eventId,
                                            DeadCallback  onDead) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (onDead == nullptr) {
        return VoidResult::Err(ErrorCode::kNullPointer);
    }

    std::uint8_t dummy = 0U;
    if (FindIndex(serviceId, eventId, dummy)) {
        return VoidResult::Err(ErrorCode::kAlreadyRegistered);
    }

    // Find a free slot
    for (std::uint8_t i = 0U; i < kMaxMonitoredSubs; ++i) {
        bool expected = false;
        if (entries_[i].active.compare_exchange_strong(expected, true,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            entries_[i].serviceId = serviceId;
            entries_[i].eventId   = eventId;
            entries_[i].onDead    = onDead;
            entries_[i].lastBeatMs.store(GetMonotonicMs(), std::memory_order_relaxed);
            entries_[i].health.store(
                static_cast<std::uint8_t>(SubscriberHealth::kAlive),
                std::memory_order_release);
            return VoidResult::Ok();
        }
    }

    return VoidResult::Err(ErrorCode::kRegistryFull);
}

// ---------------------------------------------------------------------------
// RecordHeartbeat  (lock-free fast path)
// ---------------------------------------------------------------------------

VoidResult DeadSubscriberMonitor::RecordHeartbeat(std::uint16_t serviceId,
                                                    std::uint16_t eventId) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }

    std::uint8_t idx = 0U;
    if (!FindIndex(serviceId, eventId, idx)) {
        return VoidResult::Err(ErrorCode::kServiceNotFound);
    }

    entries_[idx].lastBeatMs.store(GetMonotonicMs(), std::memory_order_release);
    entries_[idx].health.store(
        static_cast<std::uint8_t>(SubscriberHealth::kAlive),
        std::memory_order_release);

    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// ScanAndEvict
// ---------------------------------------------------------------------------

std::uint8_t DeadSubscriberMonitor::ScanAndEvict() noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return 0U;
    }

    std::uint64_t const nowMs = GetMonotonicMs();
    std::uint8_t evictedThisCycle = 0U;

    for (std::uint8_t i = 0U; i < kMaxMonitoredSubs; ++i) {
        if (!entries_[i].active.load(std::memory_order_acquire)) {
            continue;
        }

        std::uint64_t const lastBeat =
            entries_[i].lastBeatMs.load(std::memory_order_acquire);

        if (nowMs < lastBeat) {
            continue; // Clock wrap — skip
        }

        std::uint64_t const ageMs = nowMs - lastBeat;

        if (ageMs > static_cast<std::uint64_t>(kDeadTimeoutMs)) {
            // Subscriber is dead
            entries_[i].health.store(
                static_cast<std::uint8_t>(SubscriberHealth::kDead),
                std::memory_order_relaxed);

            // Invoke on-death callback before freeing the slot
            DeadCallback cb = entries_[i].onDead;
            std::uint16_t const sid = entries_[i].serviceId;
            std::uint16_t const eid = entries_[i].eventId;

            // Free the slot first (prevents re-entry issues in the callback)
            entries_[i].serviceId = kInvalidServiceId;
            entries_[i].eventId   = 0U;
            entries_[i].onDead    = nullptr;
            std::atomic_thread_fence(std::memory_order_release);
            entries_[i].active.store(false, std::memory_order_release);

            if (cb != nullptr) {
                cb(sid, eid);
            }

            evictedTotal_.fetch_add(1U, std::memory_order_relaxed);
            ++evictedThisCycle;

        } else if (ageMs > static_cast<std::uint64_t>(kHeartbeatIntervalMs)) {
            // Missed one interval — set warning
            entries_[i].health.store(
                static_cast<std::uint8_t>(SubscriberHealth::kWarning),
                std::memory_order_relaxed);
        }
    }

    return evictedThisCycle;
}

// ---------------------------------------------------------------------------
// Deregister
// ---------------------------------------------------------------------------

VoidResult DeadSubscriberMonitor::Deregister(std::uint16_t serviceId,
                                              std::uint16_t eventId) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }

    std::uint8_t idx = 0U;
    if (!FindIndex(serviceId, eventId, idx)) {
        return VoidResult::Err(ErrorCode::kServiceNotFound);
    }

    entries_[idx].serviceId = kInvalidServiceId;
    entries_[idx].eventId   = 0U;
    entries_[idx].onDead    = nullptr;
    std::atomic_thread_fence(std::memory_order_release);
    entries_[idx].active.store(false, std::memory_order_release);

    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// GetStats
// ---------------------------------------------------------------------------

MonitorStats DeadSubscriberMonitor::GetStats() const noexcept {
    MonitorStats stats{};
    stats.evictedTotal = evictedTotal_.load(std::memory_order_relaxed);

    for (std::uint8_t i = 0U; i < kMaxMonitoredSubs; ++i) {
        if (!entries_[i].active.load(std::memory_order_acquire)) {
            continue;
        }
        ++stats.totalActive;
        std::uint8_t const h =
            entries_[i].health.load(std::memory_order_relaxed);
        if (h == static_cast<std::uint8_t>(SubscriberHealth::kAlive)) {
            ++stats.totalAlive;
        } else if (h == static_cast<std::uint8_t>(SubscriberHealth::kWarning)) {
            ++stats.totalWarning;
        } else {
            ++stats.totalDead;
        }
    }

    return stats;
}

} // namespace soa
} // namespace norxs
