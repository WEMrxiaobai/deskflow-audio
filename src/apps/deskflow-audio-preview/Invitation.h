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
// enforceExpiry=false is ONLY for an already-connected endpoint and nonce.
bool verifyControl(
    const Invitation &, std::span<const std::uint8_t>, ControlType &, Nonce &, bool enforceExpiry = true
) noexcept;
} // namespace preview
