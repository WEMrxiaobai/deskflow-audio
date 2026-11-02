/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Invitation.h"
#include "audio/dsp/Pcm.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace deskflow::audio::engine {
class AudioReceiver;
}
namespace preview {
// Shared only across serialized retries for the same paired peer. Releasing the
// last owner restores local output. Never share between concurrently running sessions.
struct LocalOutputState;
std::shared_ptr<LocalOutputState> createLocalOutputState();
// Called only after the sender worker has joined. False means restoration
// failed or could not be confirmed; the watchdog retains its EOF fallback.
bool restoreLocalOutputState(const std::shared_ptr<LocalOutputState> &) noexcept;
// One playback device, one realtime mixer, and up to two independent receivers.
// prepare/attach/detach run on serialized source workers, never the GUI/audio callback.
class SharedPlayback final
{
public:
  explicit SharedPlayback(bool test = false, bool testRender = false);
  ~SharedPlayback();
  SharedPlayback(const SharedPlayback &) = delete;
  SharedPlayback &operator=(const SharedPlayback &) = delete;
  std::optional<deskflow::audio::dsp::PcmFormat> prepare(const std::atomic<bool> &stop);
  void attach(std::size_t slot, deskflow::audio::engine::AudioReceiver *receiver);
  void detach(std::size_t slot) noexcept;
  bool healthy() const noexcept;
  std::uint64_t renderedCallbacks() const noexcept;
  std::uint64_t twoSourceCallbacks() const noexcept;
  float peak() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
struct SourcePlaybackControl
{
  std::atomic<float> volume{.35f};
  std::atomic<bool> muted{false};
};
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
  double testToneHz = 440; // Explicit synthetic tests only; zero produces constant PCM.
  std::chrono::milliseconds testDuration{5000};
  std::shared_ptr<SharedPlayback> sharedPlayback;
  std::size_t playbackSlot = 0;
  std::shared_ptr<SourcePlaybackControl> playbackControl;
  std::string expectedSenderAddress; // Group receiver: IP observed by the authenticated broker.
  // Optional sender-only local output policy. Owner keeps this atomic alive until
  // worker joins. Null preserves legacy CLI/test behavior (no endpoint mutation).
  const std::atomic<bool> *localPlayback = nullptr;
  std::shared_ptr<LocalOutputState> localOutputState;
  // Paired GUI owns stop/retry over TLS; UDP teardown must not turn a retry
  // into an explicit remote stop and release retained local mute ownership.
  bool controlOwnsLifetime = false;
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
  // Receiver waits this long after acknowledging the handshake for successfully decoded media.
  std::chrono::milliseconds firstMediaTimeout{10000};
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
