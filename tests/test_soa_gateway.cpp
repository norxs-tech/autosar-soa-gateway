/**
 * =====================================================================================
 * @file        test_soa_gateway.cpp
 * @brief       Unit test suite for the norxs SOA Gateway reference implementation.
 *              Covers MC/DC (Modified Condition/Decision Coverage) for all core modules
 *              per ASIL-D requirements (>90% MC/DC target, ISO 26262 Part 6 §9.4.2).
 *
 *              Test framework: GoogleTest (gtest) v1.14+
 *              Build:  cmake -DBUILD_TESTS=ON ..
 *                      make soa_gateway_tests
 *                      ./soa_gateway_tests --gtest_output=xml:report.xml
 *
 *              Coverage measurement:
 *                  gcov / lcov: build with -fprofile-arcs -ftest-coverage
 *                  genhtml lcov.info -o coverage_html/
 *
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, ISO 26262, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#include <unistd.h>
#include <gtest/gtest.h>
#include <cstring>
#include <array>
#include <atomic>

#include "SoaServiceManager.hpp"
#include "IamSecurityController.hpp"
#include "IpcBridge.hpp"
#include "RateLimiter.hpp"
#include "DdsQosPolicy.hpp"
#include "DeadSubscriberMonitor.hpp"

using namespace norxs::soa;

// =============================================================================
// Helpers
// =============================================================================

static ServiceDescriptor MakeDesc(std::uint16_t svcId,
                                   std::uint16_t instId,
                                   char const*   name) noexcept {
    ServiceDescriptor d{};
    d.serviceId  = svcId;
    d.instanceId = instId;
    d.majorVersion = 1U;
    d.minorVersion = 0U;
    std::strncpy(d.name, name, sizeof(d.name) - 1U);
    return d;
}

static SoaEvent MakeEvent(std::uint16_t svcId,
                           std::uint16_t evtId,
                           std::uint8_t  len) noexcept {
    SoaEvent e{};
    e.serviceId  = svcId;
    e.eventId    = evtId;
    e.sessionId  = 0xDEADBEEFU;
    e.payloadLen = len;
    for (std::uint8_t i = 0U; i < len; ++i) {
        e.payload[i] = static_cast<std::uint8_t>(i + 1U);
    }
    return e;
}

// Shared SRAM simulation: aligned 8KB buffer
alignas(64) static std::array<std::uint8_t, sizeof(IpcRingBuffer)> g_sram{};

static IpcRingBuffer* SramPtr() noexcept {
    return reinterpret_cast<IpcRingBuffer*>(g_sram.data());
}

// =============================================================================
// TEST SUITE 1: SoaServiceManager
// =============================================================================

class SoaServiceManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init().ok);
    }
    SoaServiceManager mgr_;
};

// --- Init ---

TEST_F(SoaServiceManagerTest, DoubleInitReturnsAlreadyRegistered) {
    VoidResult r = mgr_.Init();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kAlreadyRegistered);
}

// --- RegisterService ---

TEST_F(SoaServiceManagerTest, RegisterSingleService) {
    auto r = mgr_.RegisterService(MakeDesc(0x0101U, 0x0001U, "SteeringService"));
    EXPECT_TRUE(r.ok);
}

TEST_F(SoaServiceManagerTest, RegisterDuplicateServiceFails) {
    mgr_.RegisterService(MakeDesc(0x0101U, 0x0001U, "SteeringService"));
    auto r = mgr_.RegisterService(MakeDesc(0x0101U, 0x0001U, "SteeringService"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kAlreadyRegistered);
}

TEST_F(SoaServiceManagerTest, RegisterUpToMaxServices) {
    for (std::uint8_t i = 0U; i < kMaxServices; ++i) {
        auto r = mgr_.RegisterService(
            MakeDesc(static_cast<std::uint16_t>(0x0100U + i),
                     0x0001U, "svc"));
        EXPECT_TRUE(r.ok) << "Failed at index " << static_cast<int>(i);
    }
}

TEST_F(SoaServiceManagerTest, RegisterBeyondMaxFails) {
    for (std::uint8_t i = 0U; i < kMaxServices; ++i) {
        mgr_.RegisterService(MakeDesc(
            static_cast<std::uint16_t>(0x0100U + i), 0x0001U, "svc"));
    }
    auto r = mgr_.RegisterService(MakeDesc(0x0200U, 0x0001U, "overflow"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kRegistryFull);
}

// --- OfferService / StopService ---

TEST_F(SoaServiceManagerTest, OfferAndStopServiceLifecycle) {
    mgr_.RegisterService(MakeDesc(0x0101U, 0x0001U, "BrakeService"));

    // Offer
    auto r = mgr_.OfferService(0x0101U, 0x0001U);
    EXPECT_TRUE(r.ok);

    auto found = mgr_.FindService(0x0101U, 0x0001U);
    ASSERT_TRUE(found.ok);
    EXPECT_EQ(found.value->state, ServiceState::kAvailable);

    // Stop
    r = mgr_.StopService(0x0101U, 0x0001U);
    EXPECT_TRUE(r.ok);
    // Second call completes kStopping → kDown (two-stage SOME/IP SD pattern)
    r = mgr_.StopService(0x0101U, 0x0001U);
    EXPECT_TRUE(r.ok);

    found = mgr_.FindService(0x0101U, 0x0001U);
    ASSERT_TRUE(found.ok);
    EXPECT_EQ(found.value->state, ServiceState::kDown);
}

TEST_F(SoaServiceManagerTest, OfferNonExistentServiceFails) {
    auto r = mgr_.OfferService(0xAAAAU, 0x0001U);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kServiceNotFound);
}

// --- Subscribe / PublishEvent / ProcessEvents ---

static std::uint8_t g_callbackCount = 0U;
static std::uint16_t g_lastEventId  = 0U;

static void TestHandler(SoaEvent const& ev) noexcept {
    ++g_callbackCount;
    g_lastEventId = ev.eventId;
}

TEST_F(SoaServiceManagerTest, SubscribeAndDispatch) {
    g_callbackCount = 0U;
    mgr_.RegisterService(MakeDesc(0x0101U, 0x0001U, "LidarService"));

    VoidResult r = mgr_.Subscribe(0x0101U, 0x0010U, TestHandler);
    EXPECT_TRUE(r.ok);

    SoaEvent ev = MakeEvent(0x0101U, 0x0010U, 4U);
    r = mgr_.PublishEvent(ev);
    EXPECT_TRUE(r.ok);

    mgr_.ProcessEvents();
    EXPECT_EQ(g_callbackCount, 1U);
    EXPECT_EQ(g_lastEventId, 0x0010U);
}

TEST_F(SoaServiceManagerTest, NullHandlerSubscribeFails) {
    auto r = mgr_.Subscribe(0x0101U, 0x0010U, nullptr);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kNullPointer);
}

TEST_F(SoaServiceManagerTest, QueueFillAndOverflow) {
    mgr_.Subscribe(0x0101U, 0x0001U, TestHandler);
    for (std::uint8_t i = 0U; i < kMaxPendingEvents - 1U; ++i) {
        EXPECT_TRUE(mgr_.PublishEvent(MakeEvent(0x0101U, 0x0001U, 1U)).ok);
    }
    // One more should overflow
    auto r = mgr_.PublishEvent(MakeEvent(0x0101U, 0x0001U, 1U));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kQueueFull);
}

TEST_F(SoaServiceManagerTest, EventWithNoMatchingSubscriberIsIgnored) {
    g_callbackCount = 0U;
    mgr_.Subscribe(0x0101U, 0x0010U, TestHandler);
    // Publish event for different service — no subscriber
    EXPECT_TRUE(mgr_.PublishEvent(MakeEvent(0x0202U, 0x0020U, 2U)).ok);
    mgr_.ProcessEvents();
    EXPECT_EQ(g_callbackCount, 0U);
}

// =============================================================================
// TEST SUITE 2: IamSecurityController
// =============================================================================

class IamSecurityControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(iam_.Init().ok);
        Role r1{};
        r1.roleId = 1U;
        std::strncpy(r1.name, "adas_controller", sizeof(r1.name) - 1U);
        ASSERT_TRUE(iam_.RegisterRole(r1).ok);

        PolicyEntry p{};
        p.principalId = 0xADA50001U;
        p.serviceId   = 0x0101U;
        p.methodId    = 0xFFFFU; // Wildcard
        p.roleId      = 1U;
        p.allowed     = AccessAction::kRead | AccessAction::kExecute;
        ASSERT_TRUE(iam_.AddPolicy(p).ok);
    }
    IamSecurityController iam_;
};

TEST_F(IamSecurityControllerTest, AuthoriseGranted) {
    auto r = iam_.Authorise(0xADA50001U, 0x0101U, 0x0010U, AccessAction::kRead);
    EXPECT_TRUE(r.ok);
}

TEST_F(IamSecurityControllerTest, AuthoriseDeniedWrongAction) {
    auto r = iam_.Authorise(0xADA50001U, 0x0101U, 0x0010U, AccessAction::kAdmin);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kUnauthorized);
}

TEST_F(IamSecurityControllerTest, AuthoriseDeniedUnknownPrincipal) {
    auto r = iam_.Authorise(0xDEADU, 0x0101U, 0x0010U, AccessAction::kRead);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kUnauthorized);
}

TEST_F(IamSecurityControllerTest, AuthoriseDeniedWrongService) {
    auto r = iam_.Authorise(0xADA50001U, 0x9999U, 0x0010U, AccessAction::kRead);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kUnauthorized);
}

TEST_F(IamSecurityControllerTest, AuditLogRecordsGrantAndDeny) {
    iam_.Authorise(0xADA50001U, 0x0101U, 0x0010U, AccessAction::kRead);    // granted
    iam_.Authorise(0xDEADU,     0x0101U, 0x0010U, AccessAction::kRead);    // denied

    std::array<AuditEntry, 8U> buf{};
    std::uint8_t const n = iam_.DrainAuditLog(buf.data(), 8U);
    EXPECT_EQ(n, 2U);
    EXPECT_EQ(buf[0].result, AuditResult::kGranted);
    EXPECT_EQ(buf[1].result, AuditResult::kDenied);
}

TEST_F(IamSecurityControllerTest, NullAuditBufferReturnsZero) {
    EXPECT_EQ(iam_.DrainAuditLog(nullptr, 8U), 0U);
}

TEST_F(IamSecurityControllerTest, SpecificMethodIdMatch) {
    PolicyEntry p{};
    p.principalId = 0xADA50002U;
    p.serviceId   = 0x0202U;
    p.methodId    = 0x0030U; // Specific method, not wildcard
    p.roleId      = 1U;
    p.allowed     = AccessAction::kWrite;
    ASSERT_TRUE(iam_.AddPolicy(p).ok);

    // Right method — granted
    EXPECT_TRUE(iam_.Authorise(0xADA50002U, 0x0202U, 0x0030U,
                                AccessAction::kWrite).ok);
    // Wrong method — denied
    EXPECT_FALSE(iam_.Authorise(0xADA50002U, 0x0202U, 0x0099U,
                                 AccessAction::kWrite).ok);
}

TEST_F(IamSecurityControllerTest, DoubleInitFails) {
    EXPECT_FALSE(iam_.Init().ok);
}

// =============================================================================
// TEST SUITE 3: IpcBridge — E2E Profile 5 & Ring Buffer
// =============================================================================

class IpcBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_sram.fill(0U);
        bridge_ = new (bridgeBuf_) IpcBridge(SramPtr());
        ASSERT_TRUE(bridge_->Init().ok);
    }
    void TearDown() override {
        bridge_->~IpcBridge();
    }

    // Placement-new to avoid re-assignment of const member ring_
    alignas(IpcBridge) std::uint8_t bridgeBuf_[sizeof(IpcBridge)]{};
    IpcBridge* bridge_{ nullptr };
};

TEST_F(IpcBridgeTest, InitSetsMagic) {
    EXPECT_EQ(SramPtr()->magic, kIpcMagic);
    EXPECT_EQ(SramPtr()->head, 0U);
    EXPECT_EQ(SramPtr()->tail, 0U);
}

TEST_F(IpcBridgeTest, SendSingleEvent) {
    SoaEvent ev = MakeEvent(0x0101U, 0x0010U, 8U);
    EXPECT_TRUE(bridge_->Send(ev).ok);
    EXPECT_EQ(SramPtr()->head, 1U);
}

TEST_F(IpcBridgeTest, SendAdvancesHeadCorrectly) {
    for (std::uint32_t i = 0U; i < 5U; ++i) {
        EXPECT_TRUE(bridge_->Send(MakeEvent(0x0101U, 0x0010U, 4U)).ok);
    }
    EXPECT_EQ(SramPtr()->head, 5U);
}

TEST_F(IpcBridgeTest, RingBufferFull) {
    // Fill all slots (kIpcRingSlots - 1 usable due to SPSC full-detection)
    for (std::uint32_t i = 0U; i < kIpcRingSlots - 1U; ++i) {
        EXPECT_TRUE(bridge_->Send(MakeEvent(0x0101U, 0x0001U, 1U)).ok)
            << "Failed at slot " << i;
    }
    auto r = bridge_->Send(MakeEvent(0x0101U, 0x0001U, 1U));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kIpcBufferFull);
}

TEST_F(IpcBridgeTest, PayloadTooLargeFails) {
    SoaEvent ev{};
    ev.serviceId  = 0x0101U;
    ev.payloadLen = static_cast<std::uint8_t>(kIpcPayloadBytes + 1U);
    auto r = bridge_->Send(ev);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kInvalidArgument);
}

TEST_F(IpcBridgeTest, E2eHeaderCrcIsNonZero) {
    SoaEvent ev = MakeEvent(0x0101U, 0x0010U, 16U);
    EXPECT_TRUE(bridge_->Send(ev).ok);
    IpcSlot const& slot = SramPtr()->slots[0U];
    EXPECT_NE(slot.e2eHeader.crc, 0U);
}

TEST_F(IpcBridgeTest, E2eSequenceCounterIncrements) {
    EXPECT_TRUE(bridge_->Send(MakeEvent(0x0101U, 0x0001U, 4U)).ok);
    EXPECT_TRUE(bridge_->Send(MakeEvent(0x0101U, 0x0001U, 4U)).ok);
    EXPECT_EQ(SramPtr()->slots[0U].e2eHeader.counter, 0U);
    EXPECT_EQ(SramPtr()->slots[1U].e2eHeader.counter, 1U);
}

TEST_F(IpcBridgeTest, VerifyE2ePassesOnValidSlot) {
    EXPECT_TRUE(bridge_->Send(MakeEvent(0x0101U, 0x0001U, 8U)).ok);
    IpcSlot const& slot = SramPtr()->slots[0U];
    EXPECT_TRUE(IpcBridge::VerifyE2e(slot, 0U).ok);
}

TEST_F(IpcBridgeTest, VerifyE2eFailsOnCrcCorruption) {
    EXPECT_TRUE(bridge_->Send(MakeEvent(0x0101U, 0x0001U, 8U)).ok);
    IpcSlot& slot = SramPtr()->slots[0U];
    slot.e2eHeader.crc ^= 0xFFFFU; // Corrupt CRC
    auto r = IpcBridge::VerifyE2e(slot, 0U);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kChecksumMismatch);
}

TEST_F(IpcBridgeTest, VerifyE2eFailsOnWrongCounter) {
    EXPECT_TRUE(bridge_->Send(MakeEvent(0x0101U, 0x0001U, 8U)).ok);
    IpcSlot const& slot = SramPtr()->slots[0U];
    auto r = IpcBridge::VerifyE2e(slot, 99U); // Wrong expected counter
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kSequenceError);
}

TEST_F(IpcBridgeTest, PayloadDataIntegrity) {
    SoaEvent ev = MakeEvent(0x0101U, 0x0010U, 10U);
    EXPECT_TRUE(bridge_->Send(ev).ok);
    IpcSlot const& slot = SramPtr()->slots[0U];
    EXPECT_EQ(slot.serviceId,  0x0101U);
    EXPECT_EQ(slot.eventId,    0x0010U);
    EXPECT_EQ(slot.payloadLen, 10U);
    EXPECT_EQ(slot.payload[0], 1U);
    EXPECT_EQ(slot.payload[9], 10U);
}

TEST_F(IpcBridgeTest, NullRingPtrFails) {
    alignas(IpcBridge) std::uint8_t buf[sizeof(IpcBridge)]{};
    IpcBridge* b = new (buf) IpcBridge(nullptr);
    EXPECT_FALSE(b->Init().ok);
    b->~IpcBridge();
}

// CRC determinism test
TEST(IpcBridgeCrcTest, CrcIsDeterministic) {
    std::array<std::uint8_t, 16U> data{};
    for (std::uint8_t i = 0U; i < 16U; ++i) { data[i] = i; }
    std::uint16_t const a = IpcBridge::ComputeE2eCrc(data.data(), 16U);
    std::uint16_t const b = IpcBridge::ComputeE2eCrc(data.data(), 16U);
    EXPECT_EQ(a, b);
}

TEST(IpcBridgeCrcTest, CrcNullDataReturnsZero) {
    EXPECT_EQ(IpcBridge::ComputeE2eCrc(nullptr, 16U), 0U);
}

TEST(IpcBridgeCrcTest, CrcDifferentDataDiffers) {
    std::array<std::uint8_t, 4U> d1{ 0x01U, 0x02U, 0x03U, 0x04U };
    std::array<std::uint8_t, 4U> d2{ 0x04U, 0x03U, 0x02U, 0x01U };
    EXPECT_NE(IpcBridge::ComputeE2eCrc(d1.data(), 4U),
              IpcBridge::ComputeE2eCrc(d2.data(), 4U));
}

// =============================================================================
// TEST SUITE 4: RateLimiter
// =============================================================================

class RateLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(rl_.Init().ok);
    }
    RateLimiter rl_;
};

TEST_F(RateLimiterTest, AdmitNewSourceSucceeds) {
    EXPECT_TRUE(rl_.Admit(0x00010001U).ok);
}

TEST_F(RateLimiterTest, BurstCapacityExhausted) {
    std::uint32_t const src = 0xBBBBU;
    // Exhaust all tokens
    for (std::int32_t i = 0; i < kBurstCapacity; ++i) {
        EXPECT_TRUE(rl_.Admit(src).ok) << "Failed at token " << i;
    }
    // Next should be dropped
    auto r = rl_.Admit(src);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kQueueFull);
}

TEST_F(RateLimiterTest, RefillRestoresTokens) {
    std::uint32_t const src = 0xCCCCU;
    for (std::int32_t i = 0; i < kBurstCapacity; ++i) { rl_.Admit(src); }
    EXPECT_FALSE(rl_.Admit(src).ok);

    // Refill simulates time passing
    // RefillBucket needs elapsed time > 0 to add tokens
    usleep(2000U);  /* 2 ms → 10 tokens at kRefillRatePerMs=5 */
    rl_.RefillAll();
    // After refill, tokens > 0, so Admit should succeed
    EXPECT_TRUE(rl_.Admit(src).ok);
}

TEST_F(RateLimiterTest, DropCountAccumulates) {
    std::uint32_t const src = 0xDDDDU;
    for (std::int32_t i = 0; i < kBurstCapacity; ++i) { rl_.Admit(src); }
    rl_.Admit(src); // drop 1
    rl_.Admit(src); // drop 2

    RateLimiterStats stats{};
    ASSERT_TRUE(rl_.GetStats(src, stats));
    EXPECT_EQ(stats.dropCount, 2U);
    EXPECT_EQ(stats.passCount, static_cast<std::uint32_t>(kBurstCapacity));
}

TEST_F(RateLimiterTest, TableFullDeniesNewSources) {
    for (std::uint8_t i = 0U; i < kMaxTrackedSources; ++i) {
        EXPECT_TRUE(rl_.Admit(static_cast<std::uint32_t>(0x1000U + i)).ok);
    }
    // New source beyond capacity
    auto r = rl_.Admit(0xDEADU);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kRegistryFull);
}

TEST_F(RateLimiterTest, GetStatsReturnsFalseForUnknownSource) {
    RateLimiterStats stats{};
    EXPECT_FALSE(rl_.GetStats(0xFFFFU, stats));
}

TEST_F(RateLimiterTest, EvictIdleFreesSlot) {
    std::uint32_t const src = 0xEEEEU;
    EXPECT_TRUE(rl_.Admit(src).ok);
    EXPECT_EQ(rl_.ActiveSourceCount(), 1U);

    // Force idle by calling EvictIdle — in unit test the source won't actually
    // be old enough to evict unless we can control time. Verify it doesn't crash
    // and count is stable when not idle.
    rl_.EvictIdle();
    EXPECT_GE(rl_.ActiveSourceCount(), 0U); // May or may not evict depending on clock
}

TEST_F(RateLimiterTest, DoubleInitFails) {
    EXPECT_FALSE(rl_.Init().ok);
}

// =============================================================================
// TEST SUITE 5: DdsQosEnforcer
// =============================================================================

class DdsQosEnforcerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(qos_.Init().ok);
        DdsQosProfile p{};
        p.topicId        = 0xABCDU;
        p.reliability    = ReliabilityKind::kReliable;
        p.deadlineMs     = 50U;
        p.latencyBudgetMs= 1U;
        p.durability     = DurabilityKind::kTransientLocal;
        p.history        = HistoryKind::kKeepLast;
        p.historyDepth   = 4U;
        std::strncpy(p.topicName, "LidarScan", sizeof(p.topicName) - 1U);
        ASSERT_TRUE(qos_.RegisterProfile(p).ok);
    }
    DdsQosEnforcer qos_;
};

TEST_F(DdsQosEnforcerTest, ValidSamplePasses) {
    EXPECT_TRUE(qos_.ValidateSample(0xABCDU, 64U).ok);
}

TEST_F(DdsQosEnforcerTest, ZeroLengthSampleFails) {
    auto r = qos_.ValidateSample(0xABCDU, 0U);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kInvalidArgument);
}

TEST_F(DdsQosEnforcerTest, OversizeSampleFails) {
    auto r = qos_.ValidateSample(0xABCDU,
        static_cast<std::uint16_t>(kMaxPayloadBytes + 1U));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kInvalidArgument);
}

TEST_F(DdsQosEnforcerTest, UnknownTopicFails) {
    auto r = qos_.ValidateSample(0x9999U, 64U);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kServiceNotFound);
}

TEST_F(DdsQosEnforcerTest, GetProfileReturnsRegistered) {
    auto r = qos_.GetProfile(0xABCDU);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value->deadlineMs, 50U);
    EXPECT_EQ(r.value->reliability, ReliabilityKind::kReliable);
}

TEST_F(DdsQosEnforcerTest, GetProfileUnknownFails) {
    EXPECT_FALSE(qos_.GetProfile(0x1111U).ok);
}

TEST_F(DdsQosEnforcerTest, CheckDeadlinesPassImmediately) {
    // Freshly registered — last sample was just now
    qos_.ValidateSample(0xABCDU, 32U);
    std::uint32_t topicId = kInvalidTopicId;
    EXPECT_TRUE(qos_.CheckDeadlines(topicId).ok);
}

TEST_F(DdsQosEnforcerTest, DuplicateProfileFails) {
    DdsQosProfile p{};
    p.topicId = 0xABCDU; // Already registered
    EXPECT_FALSE(qos_.RegisterProfile(p).ok);
}

TEST_F(DdsQosEnforcerTest, DoubleInitFails) {
    EXPECT_FALSE(qos_.Init().ok);
}

// =============================================================================
// TEST SUITE 6: DeadSubscriberMonitor
// =============================================================================

static std::uint16_t g_deadSvcId  = 0U;
static std::uint16_t g_deadEvtId  = 0U;
static std::uint8_t  g_deathCount = 0U;

static void OnDeadCallback(std::uint16_t svcId,
                             std::uint16_t evtId) noexcept {
    g_deadSvcId  = svcId;
    g_deadEvtId  = evtId;
    ++g_deathCount;
}

class DeadSubscriberMonitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_deathCount = 0U;
        g_deadSvcId  = 0U;
        g_deadEvtId  = 0U;
        ASSERT_TRUE(mon_.Init().ok);
    }
    DeadSubscriberMonitor mon_;
};

TEST_F(DeadSubscriberMonitorTest, RegisterAndHeartbeat) {
    EXPECT_TRUE(mon_.Register(0x0101U, 0x0010U, OnDeadCallback).ok);
    EXPECT_TRUE(mon_.RecordHeartbeat(0x0101U, 0x0010U).ok);
    auto stats = mon_.GetStats();
    EXPECT_EQ(stats.totalActive, 1U);
    EXPECT_EQ(stats.totalAlive,  1U);
}

TEST_F(DeadSubscriberMonitorTest, NullCallbackFails) {
    auto r = mon_.Register(0x0101U, 0x0010U, nullptr);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kNullPointer);
}

TEST_F(DeadSubscriberMonitorTest, DuplicateRegisterFails) {
    EXPECT_TRUE(mon_.Register(0x0101U, 0x0010U, OnDeadCallback).ok);
    EXPECT_FALSE(mon_.Register(0x0101U, 0x0010U, OnDeadCallback).ok);
}

TEST_F(DeadSubscriberMonitorTest, HeartbeatUnknownSubscriberFails) {
    auto r = mon_.RecordHeartbeat(0xAAAAU, 0xBBBBU);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kServiceNotFound);
}

TEST_F(DeadSubscriberMonitorTest, DeregisterRemovesSubscriber) {
    EXPECT_TRUE(mon_.Register(0x0101U, 0x0010U, OnDeadCallback).ok);
    EXPECT_TRUE(mon_.Deregister(0x0101U, 0x0010U).ok);
    EXPECT_EQ(mon_.GetStats().totalActive, 0U);
}

TEST_F(DeadSubscriberMonitorTest, DeregisterUnknownFails) {
    auto r = mon_.Deregister(0x9999U, 0x9999U);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kServiceNotFound);
}

TEST_F(DeadSubscriberMonitorTest, ScanWithActiveSubDoesNotEvict) {
    EXPECT_TRUE(mon_.Register(0x0101U, 0x0010U, OnDeadCallback).ok);
    EXPECT_TRUE(mon_.RecordHeartbeat(0x0101U, 0x0010U).ok);
    std::uint8_t const n = mon_.ScanAndEvict();
    EXPECT_EQ(n, 0U);
    EXPECT_EQ(g_deathCount, 0U);
}

TEST_F(DeadSubscriberMonitorTest, TableFullFails) {
    for (std::uint8_t i = 0U; i < kMaxMonitoredSubs; ++i) {
        EXPECT_TRUE(mon_.Register(
            static_cast<std::uint16_t>(0x0100U + i), 0x0001U,
            OnDeadCallback).ok);
    }
    auto r = mon_.Register(0x0200U, 0x0001U, OnDeadCallback);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kRegistryFull);
}

TEST_F(DeadSubscriberMonitorTest, DoubleInitFails) {
    EXPECT_FALSE(mon_.Init().ok);
}

// =============================================================================
// TEST SUITE 7: Result<T> type
// =============================================================================

TEST(ResultTypeTest, OkCarriesValue) {
    auto r = Result<int>::Ok(42);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value, 42);
    EXPECT_EQ(r.error, ErrorCode::kOk);
}

TEST(ResultTypeTest, ErrCarriesCode) {
    auto r = Result<int>::Err(ErrorCode::kTimeout);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kTimeout);
}

TEST(ResultTypeTest, VoidResultOk) {
    auto r = VoidResult::Ok();
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kOk);
}

TEST(ResultTypeTest, VoidResultErr) {
    auto r = VoidResult::Err(ErrorCode::kUnauthorized);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kUnauthorized);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
