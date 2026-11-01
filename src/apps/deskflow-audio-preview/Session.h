/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Invitation.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace preview {
enum class SessionState
{
  Waiting,
  Connecting,
  Connected,
  LocalPlaybackOverridden,
  Stopped,
  Failed
};
struct SessionEvent
{
  SessionState state;
  std::string message;
  std::uint64_t packets = 0;
};
struct SessionOptions
{
  bool receive = false;
  std::filesystem::path invitationPath;
  bool test = false;
  bool testRender = false;
  bool testCapture = false;
  // Optional sender-only local output policy. Owner keeps this atomic alive until
  // worker joins. Null preserves legacy CLI/test behavior (no endpoint mutation).
  const std::atomic<bool> *localPlayback = nullptr;
  // Sender: offer delivered over the existing authenticated Deskflow TLS channel.
  // Mutually exclusive with invitationPath; no discovery or peer authorization is
  // performed here. Caller owns and must erase this copy after the worker joins.
  std::optional<Invitation> invitation;
  // Receiver: invoked once on the worker thread AFTER binding its random UDP port.
  // The reference is valid only during this call. Deliver/copy it only to the
  // intended already-trusted peer; never log/persist it. Queue delivery and return
  // promptly: the callback must not block or wait for UDP negotiation. Exceptions
  // fail closed. An empty invitationPath selects memory-only mode and requires it.
  std::function<void(const Invitation &)> onReady;
  // Applies to both roles, independently of the offer's credential expiry.
  std::chrono::milliseconds handshakeTimeout{10000};
};
using SessionObserver = std::function<void(const SessionEvent &)>;
// Blocking control/worker-thread API: never call on the GUI thread. Callbacks run
// only on this thread, never from real-time audio callbacks. Caller authorizes
// capture/playback through its master switch and binds offers to an already-trusted
// peer using the original Deskflow pairing/TLS connection. This API never selects
// peers, authorizes identity, or displays dialogs. Legacy invitationPath remains
// available solely for explicit developer CLI/tests. Set stop and join the worker
// before destroying its owner or options. Errors emit Failed then throw; reporting
// observer exceptions are ignored, but onReady errors abort safe negotiation.
void runSession(const SessionOptions &, std::atomic<bool> &stop, const SessionObserver &observer = {});
} // namespace preview
