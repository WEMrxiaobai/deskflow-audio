/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

struct srtp_ctx_t_;

namespace deskflow::audio::transport {

inline constexpr std::size_t kMaxDatagramBytes = 1200;
inline constexpr std::size_t kRtpHeaderBytes = 12;
inline constexpr std::size_t kAuthTagBytes = 10;
inline constexpr std::size_t kMaxPayloadBytes = kMaxDatagramBytes - kRtpHeaderBytes - kAuthTagBytes;
inline constexpr std::size_t kMasterKeySaltBytes = 30; // AES-128 key (16), then master salt (14).
inline constexpr std::uint8_t kOpusPayloadType = 111;

// Supplied ONLY by the existing authenticated/authorized control session, never by UDP.
// generation is a local label: authenticated SSRC + a fresh key bind packets to this config.
// Every new stream/generation MUST get a fresh random key AND a new nonzero SSRC.
// The control layer owns authorization, key generation/distribution, uniqueness and revocation.
struct StreamConfig
{
  std::uint32_t ssrc = 0;
  std::uint64_t generation = 0;
  std::uint32_t sampleRate = 48000;
  std::uint8_t channels = 2;
  std::uint16_t frameSamples = 480; // Per channel: exactly 5 or 10 ms at 48 kHz.

  [[nodiscard]] bool valid() const noexcept;
};

// One-shot, move-only short-lived session material. No CLI, persistence or logging API.
// Construction copies exactly 30 bytes; the control caller must erase its source copy.
// Moving and consuming erase the source; destruction erases owned bytes, including on failure.
// Do NOT create a second sender with copied material: a key/SSRC/index must never be reused.
class SrtpKey final
{
public:
  explicit SrtpKey(std::span<const std::uint8_t> keyAndSalt);
  ~SrtpKey();
  SrtpKey(SrtpKey &&other) noexcept;
  SrtpKey(const SrtpKey &) = delete;
  SrtpKey &operator=(const SrtpKey &) = delete;
  SrtpKey &operator=(SrtpKey &&) = delete;

private:
  friend class SrtpSender;
  friend class SrtpReceiver;
  void clear() noexcept;
  std::array<std::uint8_t, kMasterKeySaltBytes> m_bytes{};
  bool m_available = true;
};

enum class TransportError
{
  None,
  InvalidPayload,
  PacketTooLarge,
  PacketTooShort,
  InvalidHeader,
  SsrcMismatch,
  AuthenticationFailed,
  ReplayRejected,
  CryptoFailure
};

struct ProtectResult
{
  TransportError error = TransportError::CryptoFailure;
  std::span<const std::uint8_t> packet{};
  [[nodiscard]] explicit operator bool() const noexcept
  {
    return error == TransportError::None;
  }
};

struct PacketMetadata
{
  std::uint16_t sequence = 0;
  std::uint32_t timestamp = 0; // 48 kHz RTP clock, modulo 2^32; NOT wall clock.
  std::uint32_t ssrc = 0;
  std::uint64_t generation = 0; // Trusted config, not a field accepted from the datagram.
  std::uint16_t frameSamples = 0;
};

struct ReceiveResult
{
  TransportError error = TransportError::CryptoFailure;
  std::span<const std::uint8_t> payload{};
  PacketMetadata metadata{}; // Populated only after successful authentication.
  [[nodiscard]] explicit operator bool() const noexcept
  {
    return error == TransportError::None;
  }
};

// Construct/destroy off the realtime path. Constructors may throw invalid_argument/runtime_error.
// This module owns process-wide libSRTP init/shutdown (once, shutdown at process exit).
// Other process components must not independently initialize/shut down libSRTP or unload it.
// Only packet storage is allocation-free here; libSRTP allocates its context during construction.
// Each instance has a separate libSRTP context and one owner data thread; never concurrently
// call an instance (including config/destruction). Sender and receiver may use different threads.
// No rekey/reset/switch-on-packet API: stop and replace via the trusted control plane.
// Only encoded Opus enters the network path. No PCM or unencrypted fallback exists.
// All result spans borrow fixed instance storage, invalidated by the next call/destruction.
// Input spans must not overlap this instance's previously returned borrowed storage.
class SrtpSender final
{
public:
  SrtpSender(StreamConfig config, SrtpKey &&key);
  ~SrtpSender();
  SrtpSender(const SrtpSender &) = delete;
  SrtpSender &operator=(const SrtpSender &) = delete;
  SrtpSender(SrtpSender &&) = delete;
  SrtpSender &operator=(SrtpSender &&) = delete;

  // Caller supplies sequence/timestamp. Sequence may wrap naturally; never repeat a packet
  // index or restart a sequence with this key. Timestamp increments are the caller's duty.
  [[nodiscard]] ProtectResult
  protect(std::span<const std::uint8_t> opus, std::uint16_t sequence, std::uint32_t timestamp) noexcept;
  [[nodiscard]] const StreamConfig &config() const noexcept
  {
    return m_config;
  }

private:
  const StreamConfig m_config;
  ::srtp_ctx_t_ *m_context = nullptr;
  // Spare writable trailer storage satisfies libSRTP's SRTP_MAX_TRAILER_LEN contract.
  alignas(4) std::array<std::uint8_t, kMaxDatagramBytes + 256> m_buffer{};
};

class SrtpReceiver final
{
public:
  SrtpReceiver(StreamConfig config, SrtpKey &&key);
  ~SrtpReceiver();
  SrtpReceiver(const SrtpReceiver &) = delete;
  SrtpReceiver &operator=(const SrtpReceiver &) = delete;
  SrtpReceiver(SrtpReceiver &&) = delete;
  SrtpReceiver &operator=(SrtpReceiver &&) = delete;

  // Strict RTP v2 base header, M=0, PT=111; no CSRC, extension or padding.
  // Authentication/replay/parse failure returns an empty payload and zero metadata.
  [[nodiscard]] ReceiveResult unprotect(std::span<const std::uint8_t> datagram) noexcept;
  [[nodiscard]] const StreamConfig &config() const noexcept
  {
    return m_config;
  }

private:
  const StreamConfig m_config;
  ::srtp_ctx_t_ *m_context = nullptr;
  alignas(4) std::array<std::uint8_t, kMaxDatagramBytes + 256> m_buffer{};
};

} // namespace deskflow::audio::transport
