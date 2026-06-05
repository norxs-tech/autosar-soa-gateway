/**
 * =====================================================================================
 * @file        HseAdapter.hpp
 * @brief       Hardware Security Engine (HSE) adapter for the NXP S32G SoC.
 *              Offloads cryptographic operations — SecOC CMAC/HMAC generation &
 *              verification, asymmetric certificate validation (X.509/AUTOSAR SecOC),
 *              and random number generation — from the Cortex-A53 cores to the
 *              dedicated HSE subsystem, preserving CPU cycles for real-time tasks.
 *
 *              HSE communication model (NXP HSE FW Ref Manual Rev.4):
 *                - MU (Message Unit) registers used for request/response signalling.
 *                - Shared descriptor ring located in a dedicated HSE-accessible SRAM
 *                  partition (non-cacheable, MPU-protected on A53 side).
 *                - All operations are submitted as HSE service descriptors and polled
 *                  or interrupt-driven; this implementation uses synchronous polling
 *                  for deterministic timing guarantees.
 *                - Zero dynamic allocation: descriptor pool is a static std::array.
 *
 * @project     SOA Gateway for Autonomous Safety-Supervisor
 * @standards   AUTOSAR C++14, POSIX, UN R155, ISO 21434, AUTOSAR SecOC
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        This is a reference implementation showcasing norxs's SOA architecture.
 * =====================================================================================
 */

#ifndef NORXS_SOA_HSEADAPTER_HPP
#define NORXS_SOA_HSEADAPTER_HPP

#include "SoaServiceManager.hpp"
#include <cstdint>
#include <array>
#include <atomic>

namespace norxs {
namespace soa {

// ---------------------------------------------------------------------------
// HSE constants (aligned with NXP S32G HSE Firmware Reference Manual)
// ---------------------------------------------------------------------------

static constexpr std::uint8_t  kHseMuChannel        = 0U;    ///< MU channel 0 for SOA
static constexpr std::uint8_t  kHseMaxDescriptors   = 8U;    ///< Static descriptor pool
static constexpr std::uint8_t  kHseMacLengthBytes   = 16U;   ///< AES-128-CMAC output
static constexpr std::uint8_t  kHseKeyHandleBytes   = 4U;    ///< NVM key slot handle
static constexpr std::uint16_t kHsePollTimeoutUs    = 5000U; ///< 5ms polling timeout
static constexpr std::uint8_t  kHseCertMaxSizeKb    = 4U;    ///< Max X.509 cert (4 KB)
static constexpr std::uint16_t kHseCertMaxBytes     =
    static_cast<std::uint16_t>(kHseCertMaxSizeKb * 1024U);

// ---------------------------------------------------------------------------
// HSE key slot handles (pre-provisioned in NVM during secure boot)
// ---------------------------------------------------------------------------

enum class HseKeySlot : std::uint32_t {
    kSecOcSymmetricKey  = 0x00010001U, ///< AES-128 key for CMAC (SecOC)
    kTlsRootCaKey       = 0x00020001U, ///< RSA-2048 public key for cert chain
    kEcdhEphemeralKey   = 0x00030001U, ///< ECDH P-256 ephemeral slot
    kInvalid            = 0xFFFFFFFFU
};

// ---------------------------------------------------------------------------
// HSE service operation codes (subset; full list in hse_interface.h)
// ---------------------------------------------------------------------------

enum class HseServiceId : std::uint32_t {
    kMacGenerate   = 0x00A50001U,
    kMacVerify     = 0x00A50002U,
    kHashCompute   = 0x00A50010U,
    kCertVerify    = 0x00A50020U,
    kRngGenerate   = 0x00A50030U
};

// ---------------------------------------------------------------------------
// HSE response status codes
// ---------------------------------------------------------------------------

enum class HseStatus : std::uint32_t {
    kOk               = 0x00000000U,
    kBusy             = 0x00000001U,
    kTimeout          = 0x00000002U,
    kKeyInvalid       = 0x00000010U,
    kMacMismatch      = 0x00000020U,
    kCertInvalid      = 0x00000030U,
    kGeneralError     = 0xFFFFFFFFU
};

// ---------------------------------------------------------------------------
// MAC buffer: fixed-size, stack-allocatable
// ---------------------------------------------------------------------------

struct HseMacBuffer {
    std::array<std::uint8_t, kHseMacLengthBytes> bytes{};
};

// ---------------------------------------------------------------------------
// HSE Service Descriptor (maps to NXP HSE MU descriptor format)
// Must be placed in HSE-accessible non-cached SRAM in production;
// here represented as a struct written through the HseAdapter interface.
// ---------------------------------------------------------------------------

struct HseServiceDescriptor {
    HseServiceId  serviceId  { HseServiceId::kMacGenerate };
    HseKeySlot    keySlot    { HseKeySlot::kInvalid };
    std::uint32_t inputAddr  { 0U };   ///< Physical address of input data
    std::uint32_t inputLen   { 0U };
    std::uint32_t outputAddr { 0U };   ///< Physical address of output buffer
    std::uint32_t outputLen  { 0U };
    std::uint32_t reserved[2]{};
};

static_assert(sizeof(HseServiceDescriptor) == 32U,
              "HseServiceDescriptor must be 32 bytes (aligned to HSE MU slot)");

// ---------------------------------------------------------------------------
// HseAdapter
// ---------------------------------------------------------------------------

class HseAdapter final {
public:
    HseAdapter() noexcept = default;
    ~HseAdapter() noexcept = default;

    HseAdapter(HseAdapter const&)            = delete;
    HseAdapter& operator=(HseAdapter const&) = delete;
    HseAdapter(HseAdapter&&)                 = delete;
    HseAdapter& operator=(HseAdapter&&)      = delete;

    /**
     * @brief  Initialise the HSE MU channel and verify HSE firmware is running.
     *         Reads the HSE_STATUS register via the MU SR register.
     *         Must be called once before any cryptographic operation.
     * @return VoidResult::Ok() on success; Err(kTimeout) if HSE does not respond
     *         within kHsePollTimeoutUs microseconds.
     */
    VoidResult Init() noexcept;

    /**
     * @brief  Generate a 16-byte AES-128-CMAC tag for a payload buffer.
     *         Used by NetworkAdapter and IpcBridge to stamp SecOC MACs on
     *         outbound SOME/IP frames before transmission.
     *
     * @param  data       Pointer to the message buffer (must not be nullptr).
     * @param  dataLen    Length of the message in bytes.
     * @param  keySlot    Pre-provisioned NVM key slot to use.
     * @param[out] mac    Output MAC buffer (always 16 bytes).
     * @return VoidResult::Ok() or Err(kE2eViolation / kTimeout).
     */
    VoidResult GenerateMac(std::uint8_t const* data,
                           std::uint16_t       dataLen,
                           HseKeySlot          keySlot,
                           HseMacBuffer&       mac) noexcept;

    /**
     * @brief  Verify a 16-byte AES-128-CMAC tag against a payload buffer.
     *         Used by NetworkAdapter to authenticate inbound SOME/IP frames
     *         before passing them to the RBAC firewall.
     *
     * @param  data       Pointer to the message buffer.
     * @param  dataLen    Length of the message in bytes.
     * @param  keySlot    Pre-provisioned NVM key slot.
     * @param  mac        The MAC tag to verify.
     * @return VoidResult::Ok() if authentic; Err(kE2eViolation) on mismatch;
     *         Err(kTimeout) if HSE does not respond.
     */
    VoidResult VerifyMac(std::uint8_t const* data,
                         std::uint16_t       dataLen,
                         HseKeySlot          keySlot,
                         HseMacBuffer const& mac) noexcept;

    /**
     * @brief  Verify an X.509 certificate chain against the provisioned root CA.
     *         Offloads RSA/ECDSA signature check to HSE.
     *         Used during SOME/IP TLS session establishment with external ECUs.
     *
     * @param  certDer    DER-encoded certificate bytes.
     * @param  certLen    Length in bytes (max kHseCertMaxBytes).
     * @return VoidResult::Ok() if the certificate is valid and trusted;
     *         Err(kUnauthorized) on chain failure; Err(kInvalidArgument) if
     *         certLen exceeds kHseCertMaxBytes.
     */
    VoidResult VerifyCertificate(std::uint8_t const* certDer,
                                  std::uint16_t       certLen) noexcept;

    /**
     * @brief  Generate cryptographically secure random bytes using the HSE TRNG.
     *         Used for SOME/IP session ID and nonce generation.
     *
     * @param[out] buffer   Destination buffer (must not be nullptr).
     * @param      length   Number of random bytes requested (max 64).
     * @return VoidResult::Ok() or Err(kTimeout).
     */
    VoidResult GenerateRandom(std::uint8_t* buffer,
                               std::uint8_t  length) noexcept;

    /**
     * @brief  Return whether the HSE subsystem has been successfully initialised.
     */
    bool IsReady() const noexcept;

private:
    /**
     * @brief  Submit a descriptor to the HSE MU and poll for completion.
     *         Writes to MU_TR (transmit register) and polls MU_RR (receive reg).
     * @return HseStatus from the HSE firmware response.
     */
    HseStatus SubmitAndPoll(HseServiceDescriptor const& desc) noexcept;

    /**
     * @brief  Busy-wait read of a memory-mapped register (volatile).
     */
    static std::uint32_t ReadMmioReg(std::uintptr_t addr) noexcept;

    /**
     * @brief  Write to a memory-mapped register (volatile).
     */
    static void WriteMmioReg(std::uintptr_t addr, std::uint32_t value) noexcept;

    // MU base address for the S32G3 HSE (from NXP Reference Manual §52)
    // In production this is supplied via the linker map or a platform HAL header.
    static constexpr std::uintptr_t kHseMuBaseAddr = 0x40210000UL;

    // MU register offsets
    static constexpr std::uintptr_t kMuTrReg = 0x00UL; ///< Transmit register
    static constexpr std::uintptr_t kMuRrReg = 0x10UL; ///< Receive register
    static constexpr std::uintptr_t kMuSrReg = 0x60UL; ///< Status register
    static constexpr std::uint32_t  kMuSrRdy = 0x00000008U; ///< HSE ready bit

    // Static descriptor pool (zero dynamic allocation)
    std::array<HseServiceDescriptor, kHseMaxDescriptors> descPool_{};

    // MAC scratch buffers (reused across calls, no heap)
    HseMacBuffer txMacBuf_{};
    HseMacBuffer rxMacBuf_{};

    std::atomic<bool> initialised_{ false };
};

} // namespace soa
} // namespace norxs

#endif // NORXS_SOA_HSEADAPTER_HPP
