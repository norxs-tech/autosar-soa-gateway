/**
 * =====================================================================================
 * @file        HseAdapter.cpp
 * @brief       HSE adapter implementation: MU-based descriptor submission,
 *              synchronous polling, AES-128-CMAC generation/verification,
 *              X.509 certificate chain validation, and TRNG random byte generation.
 *              All operations are non-blocking from the caller perspective but
 *              internally poll the MU receive register within a bounded iteration
 *              count derived from kHsePollTimeoutUs and the system clock frequency.
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155, ISO 21434, AUTOSAR SecOC
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#include "HseAdapter.hpp"
#include <cstring>
#include <atomic>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// MMIO helpers (volatile read/write – prevent compiler elimination)
// ---------------------------------------------------------------------------

std::uint32_t HseAdapter::ReadMmioReg(std::uintptr_t addr) noexcept {
    return *reinterpret_cast<volatile std::uint32_t const*>(addr);
}

void HseAdapter::WriteMmioReg(std::uintptr_t addr, std::uint32_t value) noexcept {
    *reinterpret_cast<volatile std::uint32_t*>(addr) = value;
}


// ---------------------------------------------------------------------------
// PhysAddr — portable virtual-to-physical address helper
// On M7 bare-metal (32-bit flat map): VA == PA, direct truncating cast.
// On QNX/A53 (64-bit VA): replace body with mem_offset() QNX API call.
// ---------------------------------------------------------------------------

static std::uint32_t PhysAddr(void const* ptr) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    // Rationale: NXP HSE MU requires 32-bit physical address. On QNX production
    // builds this must be replaced with mem_offset(ptr, 1) from <sys/mman.h>.
    // For unit-test builds on 64-bit hosts the lower 32 bits suffice for testing.
    return static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(ptr) & 0xFFFFFFFFUL);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

VoidResult HseAdapter::Init() noexcept {
    bool expected = false;
    if (!initialised_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kAlreadyRegistered);
    }

    // Zero descriptor pool
    descPool_.fill(HseServiceDescriptor{});

    // Poll HSE_STATUS via MU SR register until HSE firmware signals ready.
    // Bounded by kHsePollTimeoutUs / assumed 1us per iteration at 400MHz.
    static constexpr std::uint32_t kPollIterations = kHsePollTimeoutUs;
    for (std::uint32_t i = 0U; i < kPollIterations; ++i) {
        std::uint32_t const sr = ReadMmioReg(kHseMuBaseAddr + kMuSrReg);
        if ((sr & kMuSrRdy) != 0U) {
            return VoidResult::Ok();
        }
        // Minimal delay via memory barrier – avoids empty loop optimisation.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    // HSE did not respond – revert initialised flag.
    initialised_.store(false, std::memory_order_release);
    return VoidResult::Err(ErrorCode::kTimeout);
}

// ---------------------------------------------------------------------------
// SubmitAndPoll (private)
// Writes descriptor fields into MU transmit registers, then polls receive reg.
// In a production system the descriptor lives in shared HSE SRAM;
// we encode key fields into the MU TR registers per NXP HSE FW API §4.2.
// ---------------------------------------------------------------------------

HseStatus HseAdapter::SubmitAndPoll(HseServiceDescriptor const& desc) noexcept {
    // Write service ID to MU TR0, key slot to TR1, input addr to TR2,
    // input length to TR3 (NXP HSE MU protocol, 4-word request format).
    WriteMmioReg(kHseMuBaseAddr + kMuTrReg + 0x00UL,
                 static_cast<std::uint32_t>(desc.serviceId));
    WriteMmioReg(kHseMuBaseAddr + kMuTrReg + 0x04UL,
                 static_cast<std::uint32_t>(desc.keySlot));
    WriteMmioReg(kHseMuBaseAddr + kMuTrReg + 0x08UL, desc.inputAddr);
    WriteMmioReg(kHseMuBaseAddr + kMuTrReg + 0x0CUL, desc.inputLen);

    // Full DSB/DMB equivalent before ringing the HSE doorbell.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Poll HSE receive register for the response status word.
    static constexpr std::uint32_t kPollIterations = kHsePollTimeoutUs;
    for (std::uint32_t i = 0U; i < kPollIterations; ++i) {
        std::uint32_t const rr = ReadMmioReg(kHseMuBaseAddr + kMuRrReg);
        if (rr != static_cast<std::uint32_t>(HseStatus::kBusy)) {
            return static_cast<HseStatus>(rr);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    return HseStatus::kTimeout;
}

// ---------------------------------------------------------------------------
// GenerateMac
// ---------------------------------------------------------------------------

VoidResult HseAdapter::GenerateMac(std::uint8_t const* data,
                                    std::uint16_t       dataLen,
                                    HseKeySlot          keySlot,
                                    HseMacBuffer&       mac) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (data == nullptr) {
        return VoidResult::Err(ErrorCode::kNullPointer);
    }
    if (dataLen == 0U) {
        return VoidResult::Err(ErrorCode::kInvalidArgument);
    }

    // Zero output before request
    mac.bytes.fill(0U);

    // NXP S32G3 HSE MU descriptor requires 32-bit physical addresses.
    // PhysAddr() provides the correct translation:
    //   - M7 bare-metal: VA == PA (32-bit flat map)
    //   - QNX A53 production: replace PhysAddr() with mem_offset() QNX API
    HseServiceDescriptor desc{};
    desc.serviceId  = HseServiceId::kMacGenerate;
    desc.keySlot    = keySlot;
    desc.inputAddr  = PhysAddr(data);
    desc.inputLen   = static_cast<std::uint32_t>(dataLen);
    desc.outputAddr = PhysAddr(mac.bytes.data());
    desc.outputLen  = kHseMacLengthBytes;

    HseStatus const status = SubmitAndPoll(desc);
    if (status != HseStatus::kOk) {
        return VoidResult::Err(ErrorCode::kE2eViolation);
    }

    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// VerifyMac
// ---------------------------------------------------------------------------

VoidResult HseAdapter::VerifyMac(std::uint8_t const* data,
                                  std::uint16_t       dataLen,
                                  HseKeySlot          keySlot,
                                  HseMacBuffer const& mac) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (data == nullptr) {
        return VoidResult::Err(ErrorCode::kNullPointer);
    }

    // Copy expected MAC into the local scratch buffer to pass its address to HSE.
    rxMacBuf_ = mac;

    HseServiceDescriptor desc{};
    desc.serviceId  = HseServiceId::kMacVerify;
    desc.keySlot    = keySlot;
    desc.inputAddr  = PhysAddr(data);
    desc.inputLen   = static_cast<std::uint32_t>(dataLen);
    desc.outputAddr = PhysAddr(rxMacBuf_.bytes.data());
    desc.outputLen  = kHseMacLengthBytes;

    HseStatus const status = SubmitAndPoll(desc);

    if (status == HseStatus::kMacMismatch) {
        return VoidResult::Err(ErrorCode::kChecksumMismatch);
    }
    if (status != HseStatus::kOk) {
        return VoidResult::Err(ErrorCode::kE2eViolation);
    }

    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// VerifyCertificate
// ---------------------------------------------------------------------------

VoidResult HseAdapter::VerifyCertificate(std::uint8_t const* certDer,
                                           std::uint16_t       certLen) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (certDer == nullptr) {
        return VoidResult::Err(ErrorCode::kNullPointer);
    }
    if (certLen == 0U || certLen > kHseCertMaxBytes) {
        return VoidResult::Err(ErrorCode::kInvalidArgument);
    }

    HseServiceDescriptor desc{};
    desc.serviceId  = HseServiceId::kCertVerify;
    desc.keySlot    = HseKeySlot::kTlsRootCaKey;
    desc.inputAddr  = PhysAddr(certDer);
    desc.inputLen   = static_cast<std::uint32_t>(certLen);
    desc.outputAddr = 0U; // No output buffer for verify-only operation
    desc.outputLen  = 0U;

    HseStatus const status = SubmitAndPoll(desc);

    if (status == HseStatus::kCertInvalid) {
        return VoidResult::Err(ErrorCode::kUnauthorized);
    }
    if (status != HseStatus::kOk) {
        return VoidResult::Err(ErrorCode::kE2eViolation);
    }

    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// GenerateRandom
// ---------------------------------------------------------------------------

VoidResult HseAdapter::GenerateRandom(std::uint8_t* buffer,
                                        std::uint8_t  length) noexcept {
    if (!initialised_.load(std::memory_order_acquire)) {
        return VoidResult::Err(ErrorCode::kNotInitialized);
    }
    if (buffer == nullptr) {
        return VoidResult::Err(ErrorCode::kNullPointer);
    }
    if (length == 0U || length > 64U) {
        return VoidResult::Err(ErrorCode::kInvalidArgument);
    }

    HseServiceDescriptor desc{};
    desc.serviceId  = HseServiceId::kRngGenerate;
    desc.keySlot    = HseKeySlot::kInvalid; // TRNG — no key required
    desc.inputAddr  = 0U;
    desc.inputLen   = static_cast<std::uint32_t>(length);
    desc.outputAddr = PhysAddr(buffer);
    desc.outputLen  = static_cast<std::uint32_t>(length);

    HseStatus const status = SubmitAndPoll(desc);
    if (status != HseStatus::kOk) {
        return VoidResult::Err(ErrorCode::kTimeout);
    }

    return VoidResult::Ok();
}

// ---------------------------------------------------------------------------
// IsReady
// ---------------------------------------------------------------------------

bool HseAdapter::IsReady() const noexcept {
    return initialised_.load(std::memory_order_acquire);
}

} // namespace soa
} // namespace norxs
