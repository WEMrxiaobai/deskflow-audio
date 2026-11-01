/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "../dsp/Pcm.h"
#include "../transport/SrtpTransport.h"
#include <functional>
#include <memory>
#include <span>

namespace deskflow::audio::engine {

using dsp::PcmFormat;
using transport::SrtpKey;
using transport::StreamConfig;

// Use this monotonic clock for capture/ingest/tick, including the CLI's timer.
[[nodiscard]] std::uint64_t monotonicNowNs() noexcept;
enum class EngineState
{
  Ready,
  Running,
  Stopped,
  Failed
};
enum class EngineError
{
  None,
  WorkerException,
  CodecFailure,
  CryptoFailure,
  SequenceExhausted,
  UnsupportedPlaybackQuantum
};
struct EngineStatus
{
  EngineState state = EngineState::Ready;
  EngineError error = EngineError::None;
};

// Borrowed datagram valid only during this call. Must copy/try-send without blocking.
// Called ONLY by the encoding worker, NEVER by capture(). false means dropped, not retried.
using PacketSink = std::function<bool(std::span<const std::uint8_t>)>;
enum class SilenceEvent
{
  Entered,
  Resumed
};
struct SilenceSignal
{
  SilenceEvent event;
  std::uint64_t generation;
  std::uint16_t sequence;
  std::uint32_t timestamp;
};
// Trusted signalling transport, not UDP format discovery. On Resumed, arrange an
// authorized receiver.reanchor() BEFORE delivering resumed media. Must not block.
using SignalSink = std::function<void(const SilenceSignal &)>;
struct SenderOptions
{
  int bitrate = 160000;
  bool exactZeroGate = false; // P1 default OFF: signalling integration is not automatic.
  SignalSink silenceSignal{}; // Mandatory if exactZeroGate=true.
};
struct SenderStats
{
  std::uint64_t capturedBlocks = 0, captureDroppedBlocks = 0, expiredBlocks = 0;
  std::uint64_t sentPackets = 0, sinkDroppedPackets = 0, suppressedPackets = 0;
  std::uint64_t encodedPackets = 0;
  std::size_t queuedBlocks = 0; // Worker-published observation, capacity is always four.
  std::uint64_t timingSamples = 0, captureResidenceNsTotal = 0, captureResidenceNsMax = 0;
  std::uint64_t encodeNsTotal = 0, encodeNsMax = 0;
  bool highResolutionWait = false, coarseWaitFallback = false;
};

// One capture producer, one internal encoding/SRTP/sink worker. Construct off realtime.
// Format/key changes require stopping and constructing a NEW object with fresh key+SSRC.
// start() is one-shot: a stopped/failed sender cannot restart its SRTP packet index.
class AudioSender final
{
public:
  AudioSender(
      PcmFormat input, std::uint32_t packetMs, StreamConfig config, SrtpKey &&key, PacketSink sink,
      SenderOptions options = {}
  );
  ~AudioSender();
  AudioSender(const AudioSender &) = delete;
  AudioSender &operator=(const AudioSender &) = delete;
  void start();
  void stop() noexcept; // Cooperative; sink/signalling callbacks must never block.
  // Realtime: bounded by supplied PCM size, no allocation/locks/encoding/networking.
  // nowNs identifies the START of this callback's media on monotonicNowNs()'s clock.
  [[nodiscard]] bool capture(std::span<const float> pcm, bool discontinuity, std::uint64_t nowNs) noexcept;
  [[nodiscard]] PcmFormat inputFormat() const noexcept;
  [[nodiscard]] std::size_t adaptationLatencyInputFrames() const noexcept;
  [[nodiscard]] EngineStatus status() const noexcept;
  [[nodiscard]] SenderStats stats() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

struct ReceiverOptions
{
  float initialVolume = 0.5f;
  // Decoder-worker observation clock, queried after decode (never on the device path).
  // Optional for deterministic scheduling tests; null uses monotonicNowNs(). Must be
  // nonthrowing, return promptly in production, and share ingest/render's clock domain.
  void *feedbackClockContext = nullptr;
  std::uint64_t (*feedbackClock)(void *) noexcept = nullptr;
};
struct ReceiverStats
{
  std::uint64_t authenticatedPackets = 0, rejectedPackets = 0, jitterRejectedPackets = 0;
  std::uint64_t decodedPackets = 0, plcPackets = 0, zeroPackets = 0, decodeErrors = 0;
  std::uint64_t playbackRejectedFrames = 0, playbackTrimmedFrames = 0, underrunFrames = 0;
  std::size_t jitterPackets = 0, playbackFrames = 0;
  double correctionPpm = 0;
  std::uint64_t jitterLostPackets = 0, jitterLatePackets = 0;
  std::uint64_t timingSamples = 0, decodeNsTotal = 0, decodeNsMax = 0;
  std::uint64_t jitterResidenceNsTotal = 0, jitterResidenceNsMax = 0;
  bool requiresReanchor = false;
  std::uint64_t reanchorDiscardedMediaFrames = 0; // Decoder output withheld until reader flush ack.
  double networkClockCorrectionPpm = 0;           // Arrival/deadline mapping, not physical clock measurement.
  std::uint64_t playbackResidenceFrames = 0, playbackResidenceNsTotal = 0, playbackResidenceNsMax = 0;
  std::uint64_t feedbackUpdates = 0, feedbackConflicts = 0, feedbackInvalid = 0, feedbackCooldown = 0;
};

// Per-source decoder state. The application owns ONE decoder worker which serially
// calls ingest(), tick(), and trusted reanchor(); they are not device callback APIs.
// Authentication/parsing is inside ingest, and UDP can NEVER replace config/key/format.
// Device reader calls render()/mix(); controls and telemetry are safe cross-thread.
class AudioReceiver final
{
public:
  AudioReceiver(
      PcmFormat device, std::uint32_t packetMs, StreamConfig config, SrtpKey &&key, ReceiverOptions options = {}
  );
  ~AudioReceiver();
  AudioReceiver(const AudioReceiver &) = delete;
  AudioReceiver &operator=(const AudioReceiver &) = delete;
  [[nodiscard]] bool ingest(std::span<const std::uint8_t> datagram, std::uint64_t nowNs) noexcept;
  // feedbackNs=0 samples the clock AFTER decode, immediately before observing PCM.
  // Deterministic virtual-clock callers supply their current observation time explicitly.
  void tick(std::uint64_t nowNs, std::uint64_t feedbackNs = 0) noexcept;
  // EXPLICIT trusted control-plane media reanchor, same authorized format/key/SSRC.
  // Does not reset SRTP replay/ROC, change config or unmute. Call on decoder worker.
  // First authenticated packet automatically anchors INITIAL startup only (5ms).
  // Device reader must acknowledge old PCM flush via render/mix. Decoder output
  // before that acknowledgement is counted in reanchorDiscardedMediaFrames.
  void reanchor(std::uint16_t sequence, std::uint32_t timestamp, std::uint64_t nowNs) noexcept;
  // Optional explicit callback-entry clock, same domain as ingest/tick. Zero queries
  // monotonicNowNs(). Enables deterministic clock-domain simulation; never mix domains.
  [[nodiscard]] bool render(std::span<float> output, std::uint64_t callbackNs = 0) noexcept;
  // All sources must have matching device format and share exactly one reader.
  [[nodiscard]] static bool
  mix(std::span<AudioReceiver *const> sources, std::span<float> output, float ceiling = 0.98f) noexcept;
  void setVolume(float value) noexcept;
  void setMuted(bool value) noexcept;
  [[nodiscard]] bool muted() const noexcept;
  [[nodiscard]] PcmFormat outputFormat() const noexcept;
  [[nodiscard]] std::size_t adaptationLatencyMediaFrames() const noexcept;
  [[nodiscard]] EngineStatus status() const noexcept;
  [[nodiscard]] ReceiverStats stats() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace deskflow::audio::engine
