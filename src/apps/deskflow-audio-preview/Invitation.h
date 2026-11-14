/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#ifndef _WIN32
#error The temporary audio-preview invitation is Windows-only.
#endif

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Independent, short-lived DEV invitation, NOT Deskflow's final pairing or user
// authorization protocol. The plaintext invitation is a bearer secret: transfer
// it only through a trusted channel, do not log it, and erase/delete after use.
// HMAC proves possession, not peer identity. Caller binds Hello/Ack's nonce and
// source endpoint, rejects replay, and accepts Stop only for its active session.
// All APIs are worker/control-thread only; crypto and file operations allocate.
namespace preview {
// 邀请仍只签发十分钟；校验方额外容忍至多一分钟的墙上时钟偏差，避免对端时钟稍快时误拒绝。
// GUI 与 UDP 认证共用同一上限；此余量不延长邀请中携带的到期时间，也不允许过期凭据重新建连。
inline constexpr std::uint64_t invitationLifetimeSeconds = 600;
inline constexpr std::uint64_t invitationClockSkewSeconds = 60;
inline constexpr std::uint64_t maxInvitationRemainingSeconds = invitationLifetimeSeconds + invitationClockSkewSeconds;

struct Invitation
{
  std::array<std::uint8_t, 30> mediaKey{};
  std::array<std::uint8_t, 32> controlKey{};
  std::uint32_t ssrc = 0;
  std::uint64_t expiresUnixSeconds = 0;
  std::uint16_t port = 0;
  std::vector<std::string> addresses;
};
Invitation createInvitation(std::uint16_t port, std::vector<std::string> addresses);
// CREATE_NEW, owner-only protected DACL. Never overwrites an existing path.
void saveInvitation(const std::filesystem::path &, const Invitation &);
// Opens final component without following reparse points and rejects reparse files.
Invitation loadInvitation(const std::filesystem::path &);
// Call on all exit paths; Invitation deliberately has no implicit copying destructor.
void erase(Invitation &) noexcept;
std::uint64_t unixSeconds();

enum class ControlType : std::uint8_t
{
  Hello = 1,
  Ack = 2,
  Stop = 3
};
using Nonce = std::array<std::uint8_t, 16>;
Nonce randomNonce();
// Fresh HELLO nonce derives a fresh per-run SRTP master key/salt, even when the
// same short-lived invitation/SSRC is reopened. Never reuse a nonce with this invite.
std::array<std::uint8_t, 30> sessionMediaKey(const Invitation &, const Nonce &);
// Exactly 69 bytes: DSHAPV01(8), type(1), nonce(16), SSRC BE(4), expiry BE(8), HMAC(32).
std::vector<std::uint8_t> controlPacket(const Invitation &, ControlType, const Nonce &);
// Outputs are unchanged on failure. Stateless verification does NOT prevent replay.
// Caller must erase its Invitation even when another operation throws.
bool verifyControl(const Invitation &, std::span<const std::uint8_t>, ControlType &, Nonce &) noexcept;

// 握手完成后冻结当前会话的停止权限。邀请到期只禁止新建连接，不能使已连接会话
// 忽略停止指令。该对象只验证原端点、原 nonce 的 STOP，不接受新的 HELLO/ACK。
class EstablishedSessionControl final
{
public:
  EstablishedSessionControl(const Invitation &, const Nonce &);
  ~EstablishedSessionControl();
  EstablishedSessionControl(const EstablishedSessionControl &) = delete;
  EstablishedSessionControl &operator=(const EstablishedSessionControl &) = delete;
  [[nodiscard]] std::vector<std::uint8_t> stopPacket() const;
  [[nodiscard]] bool verifyStop(std::span<const std::uint8_t>) const noexcept;

private:
  std::array<std::uint8_t, 32> m_key{};
  Nonce m_nonce{};
  std::uint32_t m_ssrc = 0;
  std::uint64_t m_expiry = 0;
};
} // namespace preview
