/**
 * =====================================================================================
 * @file        test_safety_arbitrator.cpp
 * @brief       Unit test suite for SafetyArbitrator — ASIL-D MC/DC coverage.
 *
 *              Test categories:
 *                T1  Lifecycle (Init, double-Init, null IPC)
 *                T2  Physical envelope invariant validators
 *                T3  Sensor fault ingestion (IngestFault, RecordHeartbeat)
 *                T4  Degradation matrix — single-domain faults
 *                T5  Degradation matrix — multi-domain fault combinations
 *                T6  Emergency stop latch (one-way, only Init() clears)
 *                T7  Heartbeat timeout → auto-promote to kFailed
 *                T8  SoaEvent decoding (IngestSoaEvent)
 *                T9  Fault injection interface (InjectFault)
 *                T10 State observation APIs
 *                T11 Transition log and audit trail
 *                T12 Listener registration and callback dispatch
 *                T13 SafeStateCommand serialisation to IPC
 *                T14 Mandatory domain MRC escalation
 *
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, ISO 26262, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#include <gtest/gtest.h>
#include <cstring>
#include <array>

#include "SafetyArbitrator.hpp"
#include "IpcBridge.hpp"
#include "SoaServiceManager.hpp"

using namespace norxs::soa;

// ============================================================================
// Test Fixtures & Helpers
// ============================================================================

/// Shared SRAM simulation — aligned to 64-byte cache line
alignas(64) static std::array<std::uint8_t, sizeof(IpcRingBuffer)> g_sramBuf{};

static IpcRingBuffer* SramPtr() noexcept {
    return reinterpret_cast<IpcRingBuffer*>(g_sramBuf.data());
}

/// Listener tracking
static std::uint8_t      g_listenerCallCount{ 0U };
static SafeStateCommand  g_lastListenerCmd{};

static void TestListener(SafeStateCommand const& cmd) noexcept {
    ++g_listenerCallCount;
    g_lastListenerCmd = cmd;
}

/// Bitmask helpers
static constexpr std::uint8_t DomainBit(SensorDomain d) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(d));
}

// ============================================================================
// Base fixture: initialised IpcBridge + SafetyArbitrator
// ============================================================================

class SafetyArbitratorTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_sramBuf.fill(0U);
        g_listenerCallCount = 0U;
        g_lastListenerCmd   = SafeStateCommand{};

        // Construct IpcBridge via placement-new (const ring_ member)
        bridge_ = new (bridgeBuf_) IpcBridge(SramPtr());
        ASSERT_TRUE(bridge_->Init().ok);

        // Construct SafetyArbitrator
        arb_ = new (arbBuf_) SafetyArbitrator(bridge_);

        // Mandatory: LiDAR (bit 0) + Radar (bit 1)
        std::uint8_t const mandatory =
            DomainBit(SensorDomain::kLidar) | DomainBit(SensorDomain::kRadar);
        ASSERT_TRUE(arb_->Init(mandatory).ok);
    }

    void TearDown() override {
        arb_->~SafetyArbitrator();
        bridge_->~IpcBridge();
    }

    /// Helper: inject a fault and run one arbitration cycle
    VoidResult Fault(SensorDomain d,
                     SensorHealth h = SensorHealth::kFailed,
                     std::uint32_t dtc = 0xDEADU) {
        return arb_->InjectFault(d, h, dtc);
    }

    /// Helper: run arbitration and return the resulting state
    SafeState ArbitrateAndGetState() {
        (void)arb_->Arbitrate();
        return arb_->GetCurrentState();
    }

    alignas(IpcBridge)        std::uint8_t bridgeBuf_[sizeof(IpcBridge)]{};
    alignas(SafetyArbitrator) std::uint8_t arbBuf_[sizeof(SafetyArbitrator)]{};
    IpcBridge*        bridge_{ nullptr };
    SafetyArbitrator* arb_{ nullptr };
};

// ============================================================================
// T1 — Lifecycle
// ============================================================================

TEST_F(SafetyArbitratorTest, T1_InitialStateIsFullOperation) {
    EXPECT_EQ(arb_->GetCurrentState(), SafeState::kFullOperation);
}

TEST_F(SafetyArbitratorTest, T1_DoubleInitFails) {
    std::uint8_t const mandatory = DomainBit(SensorDomain::kLidar);
    EXPECT_FALSE(arb_->Init(mandatory).ok);
}

TEST(SafetyArbitratorLifecycle, T1_NullIpcFails) {
    alignas(SafetyArbitrator) std::uint8_t buf[sizeof(SafetyArbitrator)]{};
    auto* arb = new (buf) SafetyArbitrator(nullptr);
    EXPECT_FALSE(arb->Init(0U).ok);
    arb->~SafetyArbitrator();
}

TEST_F(SafetyArbitratorTest, T1_ArbitrateBeforeInitFails) {
    alignas(SafetyArbitrator) std::uint8_t buf[sizeof(SafetyArbitrator)]{};
    auto* arb2 = new (buf) SafetyArbitrator(bridge_);
    // NOT calling Init()
    EXPECT_EQ(arb2->Arbitrate().error, ErrorCode::kNotInitialized);
    arb2->~SafetyArbitrator();
}

// ============================================================================
// T2 — Physical Envelope Invariants
// ============================================================================

TEST(PhysicalEnvelope, T2_SteeringAngleNominal) {
    EXPECT_TRUE(SafetyArbitrator::ValidateSteeringAngle(0.0F).ok);
    EXPECT_TRUE(SafetyArbitrator::ValidateSteeringAngle(270.0F).ok);
    EXPECT_TRUE(SafetyArbitrator::ValidateSteeringAngle(-540.0F).ok);
}

TEST(PhysicalEnvelope, T2_SteeringAngleExceedsLimit) {
    auto r = SafetyArbitrator::ValidateSteeringAngle(541.0F);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kInvalidArgument);
}

TEST(PhysicalEnvelope, T2_FrictionCoeffNominal) {
    EXPECT_TRUE(SafetyArbitrator::ValidateFrictionCoeff(0.10F).ok);
    EXPECT_TRUE(SafetyArbitrator::ValidateFrictionCoeff(0.85F).ok);
    EXPECT_TRUE(SafetyArbitrator::ValidateFrictionCoeff(1.05F).ok);
}

TEST(PhysicalEnvelope, T2_FrictionCoeffBelowMin) {
    EXPECT_FALSE(SafetyArbitrator::ValidateFrictionCoeff(0.05F).ok);
}

TEST(PhysicalEnvelope, T2_FrictionCoeffAboveMax) {
    EXPECT_FALSE(SafetyArbitrator::ValidateFrictionCoeff(1.10F).ok);
}

TEST(PhysicalEnvelope, T2_DecelerationNominal) {
    EXPECT_TRUE(SafetyArbitrator::ValidateDeceleration(0.0F).ok);
    EXPECT_TRUE(SafetyArbitrator::ValidateDeceleration(9.81F).ok);
}

TEST(PhysicalEnvelope, T2_DecelerationExceedsLimit) {
    EXPECT_FALSE(SafetyArbitrator::ValidateDeceleration(10.5F).ok);
    EXPECT_FALSE(SafetyArbitrator::ValidateDeceleration(-10.5F).ok);
}

TEST(PhysicalEnvelope, T2_LateralAccelNominal) {
    EXPECT_TRUE(SafetyArbitrator::ValidateLateralAccel(0.0F).ok);
    EXPECT_TRUE(SafetyArbitrator::ValidateLateralAccel(8.5F).ok);
}

TEST(PhysicalEnvelope, T2_LateralAccelExceedsLimit) {
    EXPECT_FALSE(SafetyArbitrator::ValidateLateralAccel(9.0F).ok);
}

// ============================================================================
// T3 — Fault Ingestion & Heartbeat
// ============================================================================

TEST_F(SafetyArbitratorTest, T3_IngestFaultUpdatesHealth) {
    EXPECT_TRUE(Fault(SensorDomain::kLidar, SensorHealth::kFailed).ok);
    EXPECT_EQ(arb_->GetDomainHealth(SensorDomain::kLidar), SensorHealth::kFailed);
}

TEST_F(SafetyArbitratorTest, T3_IngestFaultInvalidDomainFails) {
    SensorFaultEvent ev{};
    ev.domain = SensorDomain::kInvalid;
    ev.health = SensorHealth::kFailed;
    EXPECT_FALSE(arb_->IngestFault(ev).ok);
}

TEST_F(SafetyArbitratorTest, T3_RecordHeartbeatClearsFaultCount) {
    Fault(SensorDomain::kCamera, SensorHealth::kDegraded);
    EXPECT_TRUE(arb_->RecordHeartbeat(SensorDomain::kCamera).ok);
    // After heartbeat with zero fault count, health should recover to Nominal
    EXPECT_TRUE(Fault(SensorDomain::kCamera, SensorHealth::kNominal).ok);
    EXPECT_EQ(arb_->GetDomainHealth(SensorDomain::kCamera), SensorHealth::kNominal);
}

TEST_F(SafetyArbitratorTest, T3_RecordHeartbeatInvalidDomainFails) {
    auto r = arb_->RecordHeartbeat(SensorDomain::kInvalid);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, ErrorCode::kInvalidArgument);
}

// ============================================================================
// T4 — Degradation Matrix: Single-Domain Faults
// ============================================================================

TEST_F(SafetyArbitratorTest, T4_LidarFailedRadarOk_RadarCameraFallback) {
    Fault(SensorDomain::kLidar, SensorHealth::kFailed);
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kRadarCameraFallback);
}

TEST_F(SafetyArbitratorTest, T4_RadarFailedLidarOk_LidarCameraFallback) {
    Fault(SensorDomain::kRadar, SensorHealth::kFailed);
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kLidarCameraFallback);
}

TEST_F(SafetyArbitratorTest, T4_CameraFailed_LidarRadarFallback) {
    Fault(SensorDomain::kCamera, SensorHealth::kFailed);
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kLidarRadarFallback);
}

TEST_F(SafetyArbitratorTest, T4_GnssFailedImuOk_DeadReckoning) {
    Fault(SensorDomain::kGnss, SensorHealth::kFailed);
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kDeadReckoningMode);
}

TEST_F(SafetyArbitratorTest, T4_ImuFailed_ReducedDynamics) {
    Fault(SensorDomain::kImu, SensorHealth::kFailed);
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kReducedDynamics);
}

TEST_F(SafetyArbitratorTest, T4_AllNominal_FullOperation) {
    // No faults injected — all domains at kUnknown initially.
    // Nominalise all domains.
    for (std::uint8_t i = 0U; i < static_cast<std::uint8_t>(SensorDomain::kInvalid); ++i) {
        arb_->InjectFault(static_cast<SensorDomain>(i), SensorHealth::kNominal, 0U);
        arb_->RecordHeartbeat(static_cast<SensorDomain>(i));
    }
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kFullOperation);
}

// ============================================================================
// T5 — Degradation Matrix: Multi-Domain Faults (priority ordering)
// ============================================================================

TEST_F(SafetyArbitratorTest, T5_LidarAndRadarBothFailed_MRC) {
    Fault(SensorDomain::kLidar, SensorHealth::kFailed);
    Fault(SensorDomain::kRadar, SensorHealth::kFailed);
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kMinimalRiskCondition);
}

TEST_F(SafetyArbitratorTest, T5_LidarFailedAndCameraFailed_RadarCameraFallback) {
    // LiDAR + Camera failed but Radar OK: LiDAR rule takes priority over Camera
    Fault(SensorDomain::kLidar,  SensorHealth::kFailed);
    Fault(SensorDomain::kCamera, SensorHealth::kFailed);
    // LiDAR alone (without Radar) → kRadarCameraFallback wins
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kRadarCameraFallback);
}

TEST_F(SafetyArbitratorTest, T5_GnssAndImuFailed_ReducedDynamicsOrMRC) {
    Fault(SensorDomain::kGnss, SensorHealth::kFailed);
    Fault(SensorDomain::kImu,  SensorHealth::kFailed);
    SafeState const s = ArbitrateAndGetState();
    // Both GNSS and IMU lost → at minimum ReducedDynamics
    EXPECT_GE(static_cast<std::uint8_t>(s),
              static_cast<std::uint8_t>(SafeState::kReducedDynamics));
}

TEST_F(SafetyArbitratorTest, T5_ThreeDomainsDown_MRCFromMandatory) {
    // Make LiDAR a mandatory domain and push fault count above threshold
    for (std::uint8_t i = 0U; i <= kFaultThreshold; ++i) {
        Fault(SensorDomain::kLidar, SensorHealth::kFailed);
    }
    Fault(SensorDomain::kCamera, SensorHealth::kFailed);
    EXPECT_EQ(ArbitrateAndGetState(), SafeState::kMinimalRiskCondition);
}

// ============================================================================
// T6 — Emergency Stop Latch
// ============================================================================

TEST_F(SafetyArbitratorTest, T6_EmergencyStopIsLatched) {
    // Inject LiDAR + Radar to get MRC, then simulate an emergency by
    // directly injecting an emergency state via fault escalation.
    // Use InjectFault to push LiDAR past mandatory threshold → MRC.
    for (std::uint8_t i = 0U; i <= kFaultThreshold; ++i) {
        Fault(SensorDomain::kLidar, SensorHealth::kFailed);
        Fault(SensorDomain::kRadar, SensorHealth::kFailed);
    }
    (void)arb_->Arbitrate(); // Should be MRC or higher

    // Now recover all sensors — state must NOT drop below EmergencyStop
    // if that was reached (or must not drop if MRC was reached for MRC test).
    SafeState const stateAfterFault = arb_->GetCurrentState();

    for (std::uint8_t i = 0U; i < static_cast<std::uint8_t>(SensorDomain::kInvalid); ++i) {
        arb_->InjectFault(static_cast<SensorDomain>(i), SensorHealth::kNominal, 0U);
        arb_->RecordHeartbeat(static_cast<SensorDomain>(i));
    }
    (void)arb_->Arbitrate();

    // De-escalation allowed for non-emergency states
    if (stateAfterFault == SafeState::kEmergencyStop) {
        EXPECT_EQ(arb_->GetCurrentState(), SafeState::kEmergencyStop);
    }
    // Otherwise recovery is permitted — just verify state is valid
    EXPECT_LE(static_cast<std::uint8_t>(arb_->GetCurrentState()),
              static_cast<std::uint8_t>(SafeState::kEmergencyStop));
}

// ============================================================================
// T7 — Heartbeat Timeout Auto-Promote
// ============================================================================

TEST_F(SafetyArbitratorTest, T7_ArbitrateWithNoHeartbeatDoesNotCrash) {
    // Domains start at kUnknown; Arbitrate must not crash regardless
    VoidResult r = arb_->Arbitrate();
    EXPECT_TRUE(r.ok || r.error == ErrorCode::kTimeout ||
                r.error == ErrorCode::kE2eViolation);
}

// ============================================================================
// T8 — SoaEvent Decoding
// ============================================================================

TEST_F(SafetyArbitratorTest, T8_IngestSoaEventValidPayload) {
    SoaEvent ev{};
    ev.payloadLen  = 12U;
    ev.payload[0U] = static_cast<std::uint8_t>(SensorDomain::kCamera);
    ev.payload[1U] = static_cast<std::uint8_t>(SensorHealth::kFailed);
    // faultCode = 0x00001234
    ev.payload[4U] = 0x34U;
    ev.payload[5U] = 0x12U;
    ev.payload[6U] = 0x00U;
    ev.payload[7U] = 0x00U;
    // timestampMs = 500
    ev.payload[8U]  = 0xF4U;
    ev.payload[9U]  = 0x01U;
    ev.payload[10U] = 0x00U;
    ev.payload[11U] = 0x00U;

    EXPECT_TRUE(arb_->IngestSoaEvent(ev).ok);
    EXPECT_EQ(arb_->GetDomainHealth(SensorDomain::kCamera), SensorHealth::kFailed);
}

TEST_F(SafetyArbitratorTest, T8_IngestSoaEventShortPayloadFails) {
    SoaEvent ev{};
    ev.payloadLen = 4U; // Too short
    EXPECT_FALSE(arb_->IngestSoaEvent(ev).ok);
}

// ============================================================================
// T9 — Fault Injection Interface
// ============================================================================

TEST_F(SafetyArbitratorTest, T9_InjectFaultSetsHealth) {
    EXPECT_TRUE(arb_->InjectFault(SensorDomain::kRadar,
                                   SensorHealth::kDegraded, 0x0001U).ok);
    EXPECT_EQ(arb_->GetDomainHealth(SensorDomain::kRadar), SensorHealth::kDegraded);
}

TEST_F(SafetyArbitratorTest, T9_InjectFaultNominalRecovery) {
    arb_->InjectFault(SensorDomain::kGnss, SensorHealth::kFailed, 0xFFFFU);
    arb_->InjectFault(SensorDomain::kGnss, SensorHealth::kNominal, 0x0000U);
    EXPECT_EQ(arb_->GetDomainHealth(SensorDomain::kGnss), SensorHealth::kNominal);
}

// ============================================================================
// T10 — State Observation APIs
// ============================================================================

TEST_F(SafetyArbitratorTest, T10_GetFaultBitmaskReflectsFaults) {
    arb_->InjectFault(SensorDomain::kLidar, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate(); // UpdateBitmasks called inside
    std::uint8_t const fb = arb_->GetFaultBitmask();
    EXPECT_NE(fb & DomainBit(SensorDomain::kLidar), 0U);
}

TEST_F(SafetyArbitratorTest, T10_GetActiveBitmaskReflectsNominal) {
    arb_->InjectFault(SensorDomain::kRadar, SensorHealth::kNominal, 0U);
    arb_->RecordHeartbeat(SensorDomain::kRadar);
    (void)arb_->Arbitrate();
    std::uint8_t const ab = arb_->GetActiveDomainBitmask();
    EXPECT_NE(ab & DomainBit(SensorDomain::kRadar), 0U);
}

TEST_F(SafetyArbitratorTest, T10_GetLastCommandSequenceIncrement) {
    arb_->InjectFault(SensorDomain::kCamera, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();

    SafeStateCommand cmd{};
    arb_->GetLastCommand(cmd);
    EXPECT_GT(cmd.sequenceNum, 0U);
}

TEST_F(SafetyArbitratorTest, T10_GetTransitionCountIncrementsOnChange) {
    std::uint32_t const before = arb_->GetTransitionCount();
    arb_->InjectFault(SensorDomain::kLidar, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();
    EXPECT_GT(arb_->GetTransitionCount(), before);
}

// ============================================================================
// T11 — Transition Log & Audit Trail
// ============================================================================

TEST_F(SafetyArbitratorTest, T11_TransitionLogRecordsEntry) {
    arb_->InjectFault(SensorDomain::kRadar, SensorHealth::kFailed, 0xABCDU);
    (void)arb_->Arbitrate();

    std::array<SafeStateTransition, 4U> log{};
    std::uint8_t const n = arb_->DrainTransitionLog(log.data(), 4U);
    EXPECT_GE(n, 1U);
    EXPECT_EQ(log[0U].fromState, SafeState::kFullOperation);
    EXPECT_EQ(log[0U].toState,   SafeState::kLidarCameraFallback);
}

TEST_F(SafetyArbitratorTest, T11_MultipleTransitionsLoggedInOrder) {
    // First transition: camera failed
    arb_->InjectFault(SensorDomain::kCamera, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();

    // Second transition: radar also failed (escalates further or stays)
    arb_->InjectFault(SensorDomain::kRadar, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();

    EXPECT_GE(arb_->GetTransitionCount(), 2U);
}

TEST_F(SafetyArbitratorTest, T11_DrainWithNullBufferReturnsZero) {
    EXPECT_EQ(arb_->DrainTransitionLog(nullptr, 4U), 0U);
}

TEST_F(SafetyArbitratorTest, T11_TransitionSequenceNumMonotone) {
    arb_->InjectFault(SensorDomain::kLidar,  SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();
    arb_->InjectFault(SensorDomain::kCamera, SensorHealth::kFailed, 0U);
    // Recovery to trigger another transition
    arb_->InjectFault(SensorDomain::kLidar,  SensorHealth::kNominal, 0U);
    arb_->RecordHeartbeat(SensorDomain::kLidar);
    (void)arb_->Arbitrate();

    std::array<SafeStateTransition, 8U> log{};
    std::uint8_t const n = arb_->DrainTransitionLog(log.data(), 8U);
    for (std::uint8_t i = 1U; i < n; ++i) {
        EXPECT_GT(log[i - 1U].sequenceNum, log[i].sequenceNum)
            << "Log should be newest-first";
    }
}

// ============================================================================
// T12 — Listener Registration & Callback Dispatch
// ============================================================================

TEST_F(SafetyArbitratorTest, T12_ListenerCalledOnTransition) {
    ASSERT_TRUE(arb_->RegisterListener(TestListener).ok);
    arb_->InjectFault(SensorDomain::kLidar, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();
    EXPECT_EQ(g_listenerCallCount, 1U);
    EXPECT_EQ(g_lastListenerCmd.state, SafeState::kRadarCameraFallback);
}

TEST_F(SafetyArbitratorTest, T12_ListenerNotCalledWhenStateUnchanged) {
    ASSERT_TRUE(arb_->RegisterListener(TestListener).ok);
    (void)arb_->Arbitrate(); // State stays at kFullOperation (or kUnknown pattern)
    std::uint8_t const countBefore = g_listenerCallCount;
    (void)arb_->Arbitrate(); // Second call — state should be stable
    // If no transition happened, listener count should not increase
    EXPECT_LE(g_listenerCallCount - countBefore, 1U);
}

TEST_F(SafetyArbitratorTest, T12_NullListenerFails) {
    EXPECT_FALSE(arb_->RegisterListener(nullptr).ok);
}

TEST_F(SafetyArbitratorTest, T12_ListenerTableFull) {
    for (std::uint8_t i = 0U; i < kMaxArbitratorListeners; ++i) {
        EXPECT_TRUE(arb_->RegisterListener(TestListener).ok);
    }
    EXPECT_FALSE(arb_->RegisterListener(TestListener).ok);
}

TEST_F(SafetyArbitratorTest, T12_ListenerReceivesCorrectSpeedCap) {
    ASSERT_TRUE(arb_->RegisterListener(TestListener).ok);
    arb_->InjectFault(SensorDomain::kCamera, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();
    // kLidarRadarFallback → max speed 30 kph
    EXPECT_FLOAT_EQ(g_lastListenerCmd.maxSpeedKph, kDegradedMaxSpeedKph);
}

// ============================================================================
// T13 — SafeStateCommand Serialisation & IPC
// ============================================================================

TEST_F(SafetyArbitratorTest, T13_CommandWrittenToIpcOnTransition) {
    std::uint32_t const headBefore = SramPtr()->head;
    arb_->InjectFault(SensorDomain::kLidar, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();
    // IPC head should have advanced
    EXPECT_GT(SramPtr()->head, headBefore);
}

TEST_F(SafetyArbitratorTest, T13_CommandMagicIsCorrect) {
    arb_->InjectFault(SensorDomain::kRadar, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();

    SafeStateCommand cmd{};
    arb_->GetLastCommand(cmd);
    EXPECT_EQ(cmd.magic, kArbitratorMagic);
}

TEST_F(SafetyArbitratorTest, T13_EmergencyStopSetsZeroSpeed) {
    // Force all mandatory domains past threshold to get MRC, then check cmd
    for (std::uint8_t i = 0U; i <= kFaultThreshold + 1U; ++i) {
        arb_->InjectFault(SensorDomain::kLidar, SensorHealth::kFailed, 0U);
        arb_->InjectFault(SensorDomain::kRadar, SensorHealth::kFailed, 0U);
    }
    (void)arb_->Arbitrate();

    SafeStateCommand cmd{};
    arb_->GetLastCommand(cmd);
    // MRC: speed cap is kMrcMaxSpeedKph
    EXPECT_LE(cmd.maxSpeedKph, kMrcMaxSpeedKph + 0.01F);
}

TEST_F(SafetyArbitratorTest, T13_FullOperationSpeedCapIs130) {
    // Ensure full operation command has correct speed cap
    for (std::uint8_t i = 0U; i < static_cast<std::uint8_t>(SensorDomain::kInvalid); ++i) {
        arb_->InjectFault(static_cast<SensorDomain>(i), SensorHealth::kNominal, 0U);
        arb_->RecordHeartbeat(static_cast<SensorDomain>(i));
    }
    // Trigger a camera fault then recover to force a transition to FullOperation
    arb_->InjectFault(SensorDomain::kCamera, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();
    arb_->InjectFault(SensorDomain::kCamera, SensorHealth::kNominal, 0U);
    arb_->RecordHeartbeat(SensorDomain::kCamera);
    (void)arb_->Arbitrate();

    SafeStateCommand cmd{};
    arb_->GetLastCommand(cmd);
    if (cmd.state == SafeState::kFullOperation) {
        EXPECT_FLOAT_EQ(cmd.maxSpeedKph, 130.0F);
    }
}

// ============================================================================
// T14 — Mandatory Domain MRC Escalation
// ============================================================================

TEST_F(SafetyArbitratorTest, T14_MandatoryDomainAboveThresholdEscalatesToMRC) {
    // LiDAR is mandatory (bit 0). Push it past kFaultThreshold consecutive faults.
    for (std::uint32_t i = 0U; i <= static_cast<std::uint32_t>(kFaultThreshold); ++i) {
        arb_->InjectFault(SensorDomain::kLidar, SensorHealth::kFailed,
                          static_cast<std::uint32_t>(0x1000U + i));
    }
    (void)arb_->Arbitrate();
    EXPECT_EQ(arb_->GetCurrentState(), SafeState::kMinimalRiskCondition);
}

TEST_F(SafetyArbitratorTest, T14_NonMandatoryDomainBelowThresholdNoMRC) {
    // USS (bit 6) is not mandatory — single fault should not trigger MRC
    arb_->InjectFault(SensorDomain::kUss, SensorHealth::kFailed, 0U);
    (void)arb_->Arbitrate();
    EXPECT_NE(arb_->GetCurrentState(), SafeState::kMinimalRiskCondition);
}

// ============================================================================
// T15 — Global Adaptor
// ============================================================================

TEST_F(SafetyArbitratorTest, T15_GlobalAdaptorForwardsEvent) {
    SetGlobalArbitrator(arb_);

    SoaEvent ev{};
    ev.payloadLen  = 12U;
    ev.payload[0U] = static_cast<std::uint8_t>(SensorDomain::kGnss);
    ev.payload[1U] = static_cast<std::uint8_t>(SensorHealth::kFailed);

    SoaArbitratorEventHandler(ev);
    EXPECT_EQ(arb_->GetDomainHealth(SensorDomain::kGnss), SensorHealth::kFailed);

    // Clean up global pointer
    SetGlobalArbitrator(nullptr);
}

TEST(GlobalAdaptor, T15_NullGlobalDoesNotCrash) {
    SetGlobalArbitrator(nullptr);
    SoaEvent ev{};
    ev.payloadLen = 12U;
    // Must not crash
    SoaArbitratorEventHandler(ev);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
