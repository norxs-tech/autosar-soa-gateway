/**
 * =====================================================================================
 * @file        SoaServiceManager.cpp
 * @brief       Implementation of the SOA Service Manager: service registry,
 *              SOME/IP SD lifecycle transitions, and lock-free event dispatch.
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#include "SoaServiceManager.hpp"
#include <cstring>

namespace norxs {
namespace soa {

SoaServiceManager::SoaServiceManager() noexcept {
    registry_.fill(ServiceDescriptor{});
    subscribers_.fill(SubscriberEntry{});
    eventQueue_.fill(SoaEvent{});
}

VoidResult SoaServiceManager::Init() noexcept {
    bool expected = false;
    if (!initialised_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kAlreadyRegistered);
    }
    registryCount_.store(0U, std::memory_order_release);
    subscriberCount_.store(0U, std::memory_order_release);
    queueHead_.store(0U, std::memory_order_release);
    queueTail_.store(0U, std::memory_order_release);
    return VoidResult::Ok();
}

bool SoaServiceManager::IsInitialised() const noexcept {
    return initialised_.load(std::memory_order_acquire);
}

bool SoaServiceManager::FindServiceIndex(std::uint16_t serviceId,
                                          std::uint16_t instanceId,
                                          std::uint8_t& outIdx) const noexcept {
    std::uint8_t const count = registryCount_.load(std::memory_order_acquire);
    for (std::uint8_t i = 0U; i < count; ++i) {
        if ((registry_[i].serviceId  == serviceId) &&
            (registry_[i].instanceId == instanceId)) {
            outIdx = i;
            return true;
        }
    }
    return false;
}

VoidResult SoaServiceManager::RegisterService(ServiceDescriptor const& desc) noexcept {
    if (!IsInitialised()) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    std::uint8_t dummy = 0U;
    if (FindServiceIndex(desc.serviceId, desc.instanceId, dummy)) {
        return VoidResult::Err(ErrorCode::kAlreadyRegistered);
    }
    std::uint8_t idx = registryCount_.load(std::memory_order_acquire);
    if (idx >= kMaxServices) {
        return VoidResult::Err(ErrorCode::kRegistryFull);
    }
    registry_[idx] = desc;
    registry_[idx].state = ServiceState::kDown;
    std::atomic_thread_fence(std::memory_order_release);
    registryCount_.store(static_cast<std::uint8_t>(idx + 1U),
                         std::memory_order_release);
    return VoidResult::Ok();
}

VoidResult SoaServiceManager::OfferService(std::uint16_t serviceId,
                                            std::uint16_t instanceId) noexcept {
    if (!IsInitialised()) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    std::uint8_t idx = 0U;
    if (!FindServiceIndex(serviceId, instanceId, idx)) {
        return VoidResult::Err(ErrorCode::kServiceNotFound);
    }
    ServiceState const current = registry_[idx].state;
    if ((current == ServiceState::kDown) || (current == ServiceState::kRequested)) {
        registry_[idx].state = ServiceState::kAvailable;
    }
    return VoidResult::Ok();
}

VoidResult SoaServiceManager::StopService(std::uint16_t serviceId,
                                           std::uint16_t instanceId) noexcept {
    if (!IsInitialised()) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    std::uint8_t idx = 0U;
    if (!FindServiceIndex(serviceId, instanceId, idx)) {
        return VoidResult::Err(ErrorCode::kServiceNotFound);
    }
    if (registry_[idx].state == ServiceState::kAvailable) {
        // Transition through kStopping to allow SOME/IP SD to broadcast
        // StopOffer before the service goes fully Down.
        // Callers observing kStopping must flush their SD StopOffer and
        // then call StopService() a second time (or the event loop drives
        // the final kDown transition after the StopOffer TTL expires).
        registry_[idx].state = ServiceState::kStopping;
    } else if (registry_[idx].state == ServiceState::kStopping) {
        // Second call: StopOffer has been sent — complete the transition.
        registry_[idx].state = ServiceState::kDown;
    }
    return VoidResult::Ok();
}

VoidResult SoaServiceManager::Subscribe(std::uint16_t serviceId,
                                         std::uint16_t eventId,
                                         EventHandler  handler) noexcept {
    if (!IsInitialised()) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (handler == nullptr) {
        return VoidResult::Err(ErrorCode::kNullPointer);
    }
    std::uint8_t idx = subscriberCount_.load(std::memory_order_acquire);
    if (idx >= kMaxSubscribers) {
        return VoidResult::Err(ErrorCode::kSubscriberFull);
    }
    subscribers_[idx].serviceId = serviceId;
    subscribers_[idx].eventId   = eventId;
    subscribers_[idx].handler   = handler;
    std::atomic_thread_fence(std::memory_order_release);
    subscriberCount_.store(static_cast<std::uint8_t>(idx + 1U),
                           std::memory_order_release);
    return VoidResult::Ok();
}

VoidResult SoaServiceManager::PublishEvent(SoaEvent const& event) noexcept {
    if (!IsInitialised()) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    std::uint8_t head = queueHead_.load(std::memory_order_relaxed);
    std::uint8_t tail = queueTail_.load(std::memory_order_acquire);
    std::uint8_t const nextHead =
        static_cast<std::uint8_t>((head + 1U) % kMaxPendingEvents);
    if (nextHead == tail) {
        return VoidResult::Err(ErrorCode::kQueueFull);
    }
    eventQueue_[head] = event;
    queueHead_.store(nextHead, std::memory_order_release);
    return VoidResult::Ok();
}

void SoaServiceManager::ProcessEvents() noexcept {
    if (!IsInitialised()) {
        return;
    }
    std::uint8_t const subCount = subscriberCount_.load(std::memory_order_acquire);
    while (true) {
        std::uint8_t tail = queueTail_.load(std::memory_order_relaxed);
        std::uint8_t head = queueHead_.load(std::memory_order_acquire);
        if (tail == head) {
            break;
        }
        SoaEvent const& ev = eventQueue_[tail];
        for (std::uint8_t i = 0U; i < subCount; ++i) {
            if ((subscribers_[i].serviceId == ev.serviceId) &&
                (subscribers_[i].eventId   == ev.eventId)   &&
                (subscribers_[i].handler   != nullptr)) {
                subscribers_[i].handler(ev);
            }
        }
        std::uint8_t const nextTail =
            static_cast<std::uint8_t>((tail + 1U) % kMaxPendingEvents);
        queueTail_.store(nextTail, std::memory_order_release);
    }
}

Result<ServiceDescriptor const*>
SoaServiceManager::FindService(std::uint16_t serviceId,
                                std::uint16_t instanceId) const noexcept {
    std::uint8_t idx = 0U;
    if (!FindServiceIndex(serviceId, instanceId, idx)) {
        return Result<ServiceDescriptor const*>::Err(ErrorCode::kServiceNotFound);
    }
    return Result<ServiceDescriptor const*>::Ok(&registry_[idx]);
}

} // namespace soa
} // namespace norxs
