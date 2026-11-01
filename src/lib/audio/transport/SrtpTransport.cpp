/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "SrtpTransport.h"

// Build with C++20 and the libSRTP 2.x install include root; link srtp2 (or its
// exported CMake target) plus that build's crypto backend/system dependencies.
// Static Windows builds may additionally need ws2_32; OpenSSL builds need crypto.
// libSRTP is BSD-3-Clause; retain its LICENSE/attribution in binary distributions.
// SECURITY: build libSRTP with ENABLE_DEBUG_LOGGING=OFF, no ERR_REPORTING_STDOUT/
// ERR_REPORTING_FILE, and never enable its runtime debug modules: upstream debug
// tracing can include session keys. Do not install a handler that logs key material.
#include <srtp2/srtp.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace deskflow::audio::transport {
namespace {

// Volatile writes cannot be removed as a dead store by an optimizing compiler.
void erase(std::span<std::uint8_t> bytes) noexcept
{
  volatile std::uint8_t *cursor = bytes.data();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    cursor[i] = 0;
  }
  std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Exactly one init/shutdown pair for this module, NOT one pair per sender/receiver.
// Function-static construction is thread safe. It precedes completion of any transport
// constructor, so static transports are destroyed before runtime shutdown as well.
// Do not call srtp_init/shutdown elsewhere: all process users must share this owner.
class SrtpRuntime final
{
public:
  SrtpRuntime()
  {
    if (srtp_init() != srtp_err_status_ok) {
      throw std::runtime_error("libSRTP initialization failed");
    }
  }
  ~SrtpRuntime()
  {
    (void)srtp_shutdown();
  }
  SrtpRuntime(const SrtpRuntime &) = delete;
  SrtpRuntime &operator=(const SrtpRuntime &) = delete;
};

void ensureRuntime()
{
  static SrtpRuntime runtime;
  (void)runtime;
}

srtp_t createContext(StreamConfig config, std::span<std::uint8_t, kMasterKeySaltBytes> key)
{
  if (!config.valid()) {
    throw std::invalid_argument("Unsupported trusted SRTP stream configuration");
  }
  ensureRuntime();
  srtp_policy_t policy{};
  srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
  srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
  policy.ssrc.type = ssrc_specific; // Never wildcard or learn a stream from an untrusted packet.
  policy.ssrc.value = config.ssrc;  // libSRTP expects this value in host byte order.
  policy.key = key.data();
  policy.window_size = 128;
  policy.allow_repeat_tx = 0;
  policy.next = nullptr;
  srtp_t context = nullptr;
  const auto status = srtp_create(&context, &policy); // Copies/derives keys during this call.
  if (status != srtp_err_status_ok) {
    if (context != nullptr) {
      (void)srtp_dealloc(context);
    }
    throw std::runtime_error("libSRTP context creation failed");
  }
  return context;
}

TransportError cryptoError(srtp_err_status_t status) noexcept
{
  switch (status) {
  case srtp_err_status_auth_fail:
    return TransportError::AuthenticationFailed;
  case srtp_err_status_replay_fail:
  case srtp_err_status_replay_old:
    return TransportError::ReplayRejected;
  default:
    return TransportError::CryptoFailure;
  }
}

void put16(std::uint8_t *out, std::uint16_t value) noexcept
{
  out[0] = static_cast<std::uint8_t>(value >> 8);
  out[1] = static_cast<std::uint8_t>(value);
}
void put32(std::uint8_t *out, std::uint32_t value) noexcept
{
  out[0] = static_cast<std::uint8_t>(value >> 24);
  out[1] = static_cast<std::uint8_t>(value >> 16);
  out[2] = static_cast<std::uint8_t>(value >> 8);
  out[3] = static_cast<std::uint8_t>(value);
}
std::uint16_t get16(const std::uint8_t *in) noexcept
{
  return static_cast<std::uint16_t>((std::uint16_t(in[0]) << 8) | in[1]);
}
std::uint32_t get32(const std::uint8_t *in) noexcept
{
  return (std::uint32_t(in[0]) << 24) | (std::uint32_t(in[1]) << 16) | (std::uint32_t(in[2]) << 8) | in[3];
}

static_assert(SRTP_MAX_TRAILER_LEN <= 256, "Increase the fixed libSRTP trailer reserve");
static_assert(kMaxDatagramBytes <= 0x7fffffff, "libSRTP takes a signed int packet length");

} // namespace

bool StreamConfig::valid() const noexcept
{
  return ssrc != 0 && generation != 0 && sampleRate == 48000 && channels == 2 &&
         (frameSamples == 240 || frameSamples == 480);
}

SrtpKey::SrtpKey(std::span<const std::uint8_t> keyAndSalt)
{
  if (keyAndSalt.size() != m_bytes.size()) {
    throw std::invalid_argument("SRTP session key and salt must be exactly 30 bytes");
  }
  std::copy(keyAndSalt.begin(), keyAndSalt.end(), m_bytes.begin());
}
SrtpKey::~SrtpKey()
{
  clear();
}
SrtpKey::SrtpKey(SrtpKey &&other) noexcept : m_bytes(other.m_bytes), m_available(other.m_available)
{
  other.clear();
}
void SrtpKey::clear() noexcept
{
  erase(m_bytes);
  m_available = false;
}

SrtpSender::SrtpSender(StreamConfig config, SrtpKey &&key) : m_config(config)
{
  SrtpKey consumed(std::move(key)); // Also erases on invalid config or failed libSRTP creation.
  if (!consumed.m_available) {
    throw std::invalid_argument("SRTP session key was already consumed");
  }
  m_context = createContext(m_config, consumed.m_bytes);
}
SrtpSender::~SrtpSender()
{
  if (m_context != nullptr) {
    (void)srtp_dealloc(m_context);
  }
  erase(m_buffer);
}

ProtectResult
SrtpSender::protect(std::span<const std::uint8_t> opus, std::uint16_t sequence, std::uint32_t timestamp) noexcept
{
  erase(m_buffer); // Invalidate any previous borrowed result, also on validation failure.
  if (opus.empty()) {
    return {TransportError::InvalidPayload, {}};
  }
  if (opus.size() > kMaxPayloadBytes) {
    return {TransportError::PacketTooLarge, {}};
  }
  m_buffer[0] = 0x80;             // V=2, P=0, X=0, CC=0.
  m_buffer[1] = kOpusPayloadType; // M=0, PT=111; no in-band format/generation negotiation.
  put16(m_buffer.data() + 2, sequence);
  put32(m_buffer.data() + 4, timestamp);
  put32(m_buffer.data() + 8, m_config.ssrc);
  std::copy(opus.begin(), opus.end(), m_buffer.begin() + kRtpHeaderBytes);
  int length = static_cast<int>(kRtpHeaderBytes + opus.size());
  const auto status = srtp_protect(m_context, m_buffer.data(), &length);
  if (status != srtp_err_status_ok) {
    erase(m_buffer);
    return {cryptoError(status), {}};
  }
  const auto expected = kRtpHeaderBytes + opus.size() + kAuthTagBytes;
  if (length < 0 || static_cast<std::size_t>(length) != expected || expected > kMaxDatagramBytes) {
    erase(m_buffer);
    return {TransportError::CryptoFailure, {}};
  }
  return {TransportError::None, {m_buffer.data(), expected}};
}

SrtpReceiver::SrtpReceiver(StreamConfig config, SrtpKey &&key) : m_config(config)
{
  SrtpKey consumed(std::move(key));
  if (!consumed.m_available) {
    throw std::invalid_argument("SRTP session key was already consumed");
  }
  m_context = createContext(m_config, consumed.m_bytes);
}
SrtpReceiver::~SrtpReceiver()
{
  if (m_context != nullptr) {
    (void)srtp_dealloc(m_context);
  }
  erase(m_buffer);
}

ReceiveResult SrtpReceiver::unprotect(std::span<const std::uint8_t> datagram) noexcept
{
  erase(m_buffer);
  if (datagram.size() > kMaxDatagramBytes) {
    return {TransportError::PacketTooLarge, {}, {}};
  }
  if (datagram.size() < kRtpHeaderBytes + kAuthTagBytes + 1) {
    return {TransportError::PacketTooShort, {}, {}};
  }
  // These are rejection-only prechecks of UNTRUSTED bytes, not accepted metadata.
  if (datagram[0] != 0x80 || datagram[1] != kOpusPayloadType) {
    return {TransportError::InvalidHeader, {}, {}};
  }
  if (get32(datagram.data() + 8) != m_config.ssrc) {
    return {TransportError::SsrcMismatch, {}, {}};
  }
  std::copy(datagram.begin(), datagram.end(), m_buffer.begin());
  int length = static_cast<int>(datagram.size());
  const auto status = srtp_unprotect(m_context, m_buffer.data(), &length);
  if (status != srtp_err_status_ok) {
    erase(m_buffer); // Never expose partially decrypted or unauthenticated output.
    return {cryptoError(status), {}, {}};
  }
  if (length <= static_cast<int>(kRtpHeaderBytes) || length != static_cast<int>(datagram.size() - kAuthTagBytes) ||
      m_buffer[0] != 0x80 || m_buffer[1] != kOpusPayloadType || get32(m_buffer.data() + 8) != m_config.ssrc) {
    erase(m_buffer);
    return {TransportError::InvalidHeader, {}, {}};
  }
  const PacketMetadata metadata{
      get16(m_buffer.data() + 2), get32(m_buffer.data() + 4), m_config.ssrc, m_config.generation, m_config.frameSamples
  };
  return {
      TransportError::None,
      {m_buffer.data() + kRtpHeaderBytes, static_cast<std::size_t>(length) - kRtpHeaderBytes},
      metadata
  };
}

} // namespace deskflow::audio::transport
