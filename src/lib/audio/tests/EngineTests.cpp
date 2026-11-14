/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "audio/codec/OpusCodec.h"
#include "audio/engine/AudioEngine.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using namespace deskflow::audio;
using namespace deskflow::audio::engine;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t ms = 1000000;
void require(bool value, const char *message)
{
  if (!value) {
    throw std::runtime_error(message);
  }
}
// Synthetic public TEST material, deliberately not a production key generator.
SrtpKey key()
{
  std::array<std::uint8_t, 30> material{};
  for (std::size_t i = 0; i < material.size(); ++i) {
    material[i] = static_cast<std::uint8_t>(i * 7 + 11);
  }
  return SrtpKey(material);
}
StreamConfig config(std::uint16_t frames = 240)
{
  return {0x10203040, 1, 48000, 2, frames};
}
std::array<float, 480> tone(float level = 0.0001f)
{
  std::array<float, 480> result{};
  for (std::size_t frame = 0; frame < result.size() / 2; ++frame) {
    result[frame * 2] = result[frame * 2 + 1] = level * std::sin(float(frame) * 0.13f);
  }
  return result;
}
// Deliberately blocking test fixture, NOT a production PacketSink. Capture remains
// independent while the test holds the worker; stop is called only after release.
struct PacketBox
{
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Bytes> packets;
  std::thread::id sinkThread{};
  bool block = false, entered = false;
  bool put(std::span<const std::uint8_t> packet)
  {
    std::unique_lock lock(mutex);
    entered = true;
    sinkThread = std::this_thread::get_id();
    packets.emplace_back(packet.begin(), packet.end());
    changed.notify_all();
    changed.wait(lock, [&] { return !block; });
    return true;
  }
  void wait(std::size_t count)
  {
    std::unique_lock lock(mutex);
    require(
        changed.wait_for(lock, std::chrono::seconds(3), [&] { return packets.size() >= count; }),
        "Timed out waiting for independent encoding worker"
    );
  }
  void release()
  {
    std::lock_guard lock(mutex);
    block = false;
    changed.notify_all();
  }
};
void asyncAuthenticatedLoopback()
{
  PacketBox box;
  AudioSender sender({48000, 2}, 5, config(), key(), [&](auto packet) { return box.put(packet); });
  require(sender.status().state == EngineState::Ready, "Sender should initially be Ready");
  sender.start();
  require(sender.capture(tone(), false, monotonicNowNs()), "Low-level capture rejected");
  box.wait(1);
  sender.stop();
  require(box.sinkThread != std::this_thread::get_id(), "Sink/codec ran on capture thread");
  require(sender.stats().sentPackets == 1 && sender.stats().encodedPackets == 1, "Missing sender stats");
  require(sender.stats().suppressedPackets == 0, "Low-level samples were noise gated");
  AudioReceiver receiver({48000, 2}, 5, config(), key(), ReceiverOptions{1});
  const auto now = monotonicNowNs();
  auto damaged = box.packets.front();
  damaged.back() ^= 0x20;
  require(!receiver.ingest(damaged, now), "Unauthenticated SRTP accepted");
  require(receiver.ingest(box.packets.front(), now), "Authenticated memory loopback failed");
  require(!receiver.ingest(box.packets.front(), now), "SRTP replay accepted");
  receiver.tick(now + 4 * ms);
  require(receiver.stats().decodedPackets == 0, "Startup was earlier than independent 5ms delay");
  receiver.tick(now + 5 * ms);
  require(receiver.stats().decodedPackets == 1, "Jitter startup incorrectly waited for eight packets");
  std::array<float, 480> output{};
  require(receiver.render(output), "Render rejected device format");
  require(
      std::any_of(output.begin(), output.end(), [](float sample) { return sample != 0; }),
      "Low-level signal disappeared in engine"
  );
  require(receiver.stats().rejectedPackets == 2, "Authentication/replay rejection stats missing");
  require(receiver.stats().timingSamples == 1, "Decode timing missing");
}

struct DirectPackets
{
  codec::OpusCodec codec{240};
  transport::SrtpSender sender{config(), key()};
  // 可指定扩展序号对应的媒体时间戳，用于序号回绕而媒体时间线连续的回归。
  Bytes packet(std::uint16_t sequence, std::optional<std::uint32_t> timestamp = std::nullopt)
  {
    std::array<std::uint8_t, transport::kMaxPayloadBytes> encoded{};
    const auto count = codec.encode(tone(0.1f), encoded);
    require(count > 0, "Test Opus encode failed");
    const auto result = sender.protect(
        std::span<const std::uint8_t>(encoded.data(), count), sequence,
        timestamp.value_or(std::uint32_t(sequence) * 240)
    );
    require(bool(result), "Test SRTP protect failed");
    return {result.packet.begin(), result.packet.end()};
  }
};
void callbackBetweenDecodeAndObservationMakesProgress()
{
  struct Clock
  {
    std::binary_semaphore request{0}, complete{0};
    std::atomic<std::uint64_t> now{0};
    std::atomic<bool> stop{false}, renderOk{true};
    unsigned observations = 0; // Decoder owned.
    static std::uint64_t read(void *context) noexcept
    {
      auto &clock = *static_cast<Clock *>(context);
      clock.request.release();
      clock.complete.acquire(); // Test-only deterministic interleaving, never production.
      ++clock.observations;
      // First establish fully valid progress. Then deliberately alternate a stale
      // clock sample with valid evidence (same conflict status as a concurrent begin).
      const bool stale = clock.observations > 120 && (clock.observations % 2) == 0;
      return clock.now.load(std::memory_order_acquire) + (stale ? -1000LL : 1000LL);
    }
  } clock;
  ReceiverOptions options;
  options.feedbackClockContext = &clock;
  options.feedbackClock = &Clock::read;
  DirectPackets packets;
  AudioReceiver receiver({48000, 2}, 5, config(), key(), options);
  std::thread device([&] {
    std::array<float, 480> output{};
    for (;;) {
      clock.request.acquire();
      if (clock.stop.load(std::memory_order_acquire))
        break;
      if (!receiver.render(output, clock.now.load(std::memory_order_acquire)))
        clock.renderOk = false;
      clock.complete.release();
    }
  });
  // Reader runs only after tick has decoded, before the worker observes the clock.
  // It publishes a callback timestamp LATER than tick entry, the old starvation case.
  bool ingested = true;
  constexpr std::uint64_t start = 1000000000000ULL;
  ReceiverStats firstPhase{};
  for (unsigned index = 0; index < 240; ++index) {
    const auto arrival = start + index * 5 * ms;
    ingested = receiver.ingest(packets.packet(static_cast<std::uint16_t>(index)), arrival) && ingested;
    clock.now.store(arrival + 5 * ms + 1000, std::memory_order_release);
    receiver.tick(arrival + 5 * ms);
    if (index == 119)
      firstPhase = receiver.stats();
  }
  clock.stop.store(true, std::memory_order_release);
  clock.request.release();
  device.join();
  require(ingested && clock.renderOk, "Deterministic callback fixture failed");
  const auto stats = receiver.stats();
  require(firstPhase.feedbackUpdates > 80, "Completed interleaved callbacks starved feedback progress");
  require(firstPhase.feedbackConflicts == 0, "Post-decode clock still predates completed callback");
  require(stats.feedbackConflicts >= 50, "Alternating conflict fixture did not reject evidence");
  require(
      stats.feedbackUpdates - firstPhase.feedbackUpdates > 45,
      "Alternating conflicts repeatedly cleared baseline and starved PI"
  );
  // Re-anchor with no intervening callback: old PCM must stay withheld until a
  // NONEMPTY device read. Empty render/mix must not acknowledge that flush.
  receiver.reanchor(400, 400 * 240, start + 2000 * ms);
  const auto before = receiver.stats();
  require(receiver.render({}, start + 10000 * ms), "Empty render rejected");
  std::array<AudioReceiver *, 1> source{&receiver};
  require(AudioReceiver::mix(source, {}), "Empty mix rejected");
  const auto after = receiver.stats();
  require(
      before.underrunFrames == after.underrunFrames && before.playbackTrimmedFrames == after.playbackTrimmedFrames,
      "Empty output consumed or trimmed PCM"
  );
  require(receiver.ingest(packets.packet(400), start + 2000 * ms), "Empty flush fixture ingest failed");
  receiver.tick(start + 2005 * ms, start + 2005 * ms);
  require(
      receiver.stats().reanchorDiscardedMediaFrames == 240, "Empty output incorrectly acknowledged stale PCM flush"
  );
}
void lossFreezesPlaybackFeedback()
{
  DirectPackets packets;
  AudioReceiver receiver({48000, 2}, 5, config(), key());
  constexpr std::uint64_t start = 1000000000000ULL;
  std::array<float, 480> output{};
  double beforeLoss = 0;
  std::uint64_t updatesBeforeLoss = 0;
  for (unsigned index = 0; index < 20; ++index) {
    const auto arrival = start + index * 5 * ms;
    if (index != 10) {
      require(
          receiver.ingest(packets.packet(static_cast<std::uint16_t>(index)), arrival),
          "Feedback-loss fixture ingest failed"
      );
    }
    receiver.tick(arrival + 5 * ms, arrival + 5 * ms);
    require(receiver.render(output, arrival + 5 * ms + 1000), "Feedback-loss fixture render failed");
    if (index == 9) {
      beforeLoss = receiver.stats().correctionPpm;
      updatesBeforeLoss = receiver.stats().feedbackUpdates;
    }
    if (index >= 10 && index <= 16) {
      require(
          receiver.stats().correctionPpm == beforeLoss && receiver.stats().feedbackUpdates == updatesBeforeLoss,
          "Loss/recovery samples steered frequency during cooldown"
      );
    }
  }
  require(receiver.stats().plcPackets == 1, "Feedback-loss fixture did not exercise PLC");
  require(
      updatesBeforeLoss > 0 && receiver.stats().feedbackCooldown > 0 &&
          receiver.stats().feedbackUpdates > updatesBeforeLoss,
      "Feedback did not run before loss, freeze, and recover afterwards"
  );
}
void boundedPlcAndMute()
{
  DirectPackets packets;
  AudioReceiver receiver({48000, 2}, 5, config(), key(), ReceiverOptions{1});
  const auto now = monotonicNowNs();
  require(receiver.ingest(packets.packet(0), now), "Initial packet rejected");
  receiver.tick(now + 5 * ms);
  std::array<float, 480> output{};
  require(receiver.render(output), "Initial render failed");
  for (unsigned index = 1; index <= 12; ++index) {
    receiver.tick(now + (5 + index * 5) * ms);
    require(receiver.render(output), "PLC render failed");
  }
  require(receiver.stats().plcPackets == 2, "PLC must stop after two consecutive losses");
  require(receiver.stats().zeroPackets >= 8, "Exhausted PLC failed to produce zeros");
  require(
      std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0; }),
      "Loss tail repeated audio indefinitely"
  );
  require(receiver.stats().playbackFrames <= 960, "Playback exceeded 20ms capacity");
  receiver.setMuted(true);
  receiver.reanchor(20, 20 * 240, now + 100 * ms);
  require(receiver.muted(), "Trusted reconnect unmuted the user");
  // Reader acknowledges the old-ring flush before the writer publishes resumed PCM.
  require(receiver.render(output), "Flush render failed");
  require(receiver.ingest(packets.packet(20), now + 100 * ms), "Resumed authenticated packet rejected");
  receiver.tick(now + 105 * ms);
  require(receiver.render(output), "Muted render failed");
  require(receiver.muted(), "Ingest changed mute");
  // Smooth mute is 5ms, so after this one full block the target must be exact zero.
  require(receiver.render(output), "Mute tail render failed");
  require(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0; }), "Mute did not hold");
  receiver.tick(now + 1000 * ms);
  require(receiver.stats().requiresReanchor, "Severe worker stall should require trusted reanchor");
}
void overloadBoundedAndMediaTimeline()
{
  PacketBox box;
  box.block = true;
  AudioSender sender({48000, 2}, 5, config(), key(), [&](auto packet) { return box.put(packet); });
  sender.start();
  require(sender.capture(tone(), false, monotonicNowNs()), "Initial capture failed");
  box.wait(1);
  // While encoder/sink is held, capture must return immediately and reject newest
  // blocks beyond its FOUR slots, without waiting for worker/network progress.
  for (unsigned index = 0; index < 100; ++index) {
    require(sender.capture(tone(), false, monotonicNowNs()), "Capture callback rejected valid PCM");
  }
  require(sender.stats().captureDroppedBlocks >= 96, "Capture queue is not bounded to four blocks");
  box.release();
  // Wait for consumer to discard its old backlog before submitting the next block.
  const auto deadline = monotonicNowNs() + 3'000 * ms;
  while (sender.stats().expiredBlocks < 4 && monotonicNowNs() < deadline) {
    std::this_thread::yield();
  }
  require(sender.stats().expiredBlocks >= 4, "Consumer did not discard stale PCM");
  require(sender.capture(tone(), true, monotonicNowNs()), "Post-overload capture failed");
  box.wait(2);
  sender.stop();
  transport::SrtpReceiver verify(config(), key());
  const auto first = verify.unprotect(box.packets[0]);
  require(bool(first), "First packet failed SRTP auth");
  const auto firstSequence = first.metadata.sequence;
  const auto second = verify.unprotect(box.packets[1]);
  require(bool(second), "Post-overload packet failed SRTP auth");
  require(second.metadata.sequence > firstSequence + 4, "Dropped media sequence slots were reused");
  require(
      second.metadata.timestamp == std::uint32_t(second.metadata.sequence) * 240,
      "RTP timestamp no longer agrees with dropped media sequence slots"
  );
}
void exactZeroGateSignals()
{
  PacketBox box;
  std::atomic<unsigned> entered{0}, resumed{0};
  SenderOptions options;
  options.exactZeroGate = true;
  options.silenceSignal = [&](const SilenceSignal &signal) {
    if (signal.event == SilenceEvent::Entered) {
      entered.fetch_add(1, std::memory_order_relaxed);
    } else {
      resumed.fetch_add(1, std::memory_order_relaxed);
    }
  };
  AudioSender sender({48000, 2}, 5, config(), key(), [&](auto packet) { return box.put(packet); }, options);
  sender.start();
  require(sender.capture(tone(0.000001f), false, monotonicNowNs()), "Enabled gate rejected low level");
  box.wait(1);
  const std::array<float, 480> zeros{};
  for (unsigned index = 0; index < 39; ++index) {
    require(sender.capture(zeros, false, monotonicNowNs()), "Exact-zero capture failed");
    box.wait(index + 2);
  }
  require(entered.load(std::memory_order_relaxed) == 0, "Gate suppressed before 200ms exact zeros");
  require(sender.capture(zeros, false, monotonicNowNs()), "200ms zero capture failed");
  const auto deadline = monotonicNowNs() + 3'000 * ms;
  while (sender.stats().suppressedPackets == 0 && monotonicNowNs() < deadline) {
    std::this_thread::yield();
  }
  require(
      sender.stats().suppressedPackets == 1 && entered.load(std::memory_order_relaxed) == 1,
      "Gate did not explicitly signal entry at 200ms"
  );
  require(sender.capture(tone(0.000001f), false, monotonicNowNs()), "Gate resume capture failed");
  box.wait(41);
  sender.stop();
  require(resumed.load(std::memory_order_relaxed) == 1, "Gate resume missing trusted signalling callback");
  transport::SrtpReceiver verify(config(), key());
  std::uint16_t previousSequence = 0;
  for (std::size_t i = 0; i < box.packets.size(); ++i) {
    const auto received = verify.unprotect(box.packets[i]);
    require(bool(received), "Gated/resumed SRTP packet failed authentication");
    if (i + 1 == box.packets.size()) {
      require(received.metadata.sequence == previousSequence + 2, "Silence resume reused skipped SRTP sequence");
    }
    previousSequence = received.metadata.sequence;
  }
}
void workerExceptionAndGateContract()
{
  bool rejected = false;
  try {
    SenderOptions options;
    options.exactZeroGate = true;
    AudioSender invalid({48000, 2}, 5, config(), key(), [](auto) { return true; }, options);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "Gate enabled without explicit trusted signalling callback");
  AudioSender throwing({48000, 2}, 5, config(), key(), [](auto) -> bool { throw std::runtime_error("test sink"); });
  throwing.start();
  require(throwing.capture(tone(), false, monotonicNowNs()), "Throw test capture failed");
  const auto deadline = monotonicNowNs() + 3'000 * ms;
  while (throwing.status().state != EngineState::Failed && monotonicNowNs() < deadline) {
    std::this_thread::yield();
  }
  throwing.stop();
  require(
      throwing.status().state == EngineState::Failed && throwing.status().error == EngineError::WorkerException,
      "Worker exception escaped or failure state was lost"
  );
}
void trustedReanchorFlushAndPlaybackCap()
{
  DirectPackets packets;
  AudioReceiver receiver({48000, 2}, 5, config(), key(), ReceiverOptions{1});
  const auto now = monotonicNowNs();
  require(receiver.ingest(packets.packet(0), now), "Flush initial packet rejected");
  receiver.tick(now + 5 * ms);
  receiver.reanchor(1, 240, now + 10 * ms);
  require(receiver.ingest(packets.packet(1), now + 10 * ms), "Flush resumed packet rejected");
  receiver.tick(now + 15 * ms); // Reader has not acknowledged old PCM flush yet.
  require(
      receiver.stats().reanchorDiscardedMediaFrames == 240,
      "Reanchor handshake discard must be explicit telemetry, not hidden queue rejection"
  );
  std::array<float, 480> output{};
  require(receiver.render(output), "Flush acknowledge render failed");
  for (std::uint16_t sequence = 2; sequence < 12; ++sequence) {
    const auto arrival = now + std::uint64_t(5 + sequence * 5) * ms;
    require(receiver.ingest(packets.packet(sequence), arrival), "Playback cap packet rejected");
    receiver.tick(arrival + 5 * ms);
  }
  require(receiver.stats().playbackFrames <= 1440, "Decoded playback ring exceeded its 30ms cap");
  require(receiver.stats().playbackRejectedFrames > 0, "Full playback ring should reject new PCM boundedly");
  require(receiver.render(output), "Latency trim render failed");
  require(receiver.stats().playbackTrimmedFrames >= 200, "Reader did not trim playback backlog above 25ms");
}
void unsupportedPlaybackQuantumFailsClearly()
{
  AudioReceiver receiver({48000, 2}, 5, config(), key());
  std::array<float, 962> tooLarge{};
  tooLarge.fill(.5f);
  require(!receiver.render(tooLarge), "Unsupported >10ms callback was silently accepted");
  require(
      receiver.status().state == EngineState::Failed &&
          receiver.status().error == EngineError::UnsupportedPlaybackQuantum,
      "Unsupported callback granularity missing explicit status"
  );
  require(
      std::all_of(tooLarge.begin(), tooLarge.end(), [](float value) { return value == 0; }),
      "Unsupported callback leaked old output"
  );
  AudioReceiver good({48000, 2}, 5, config(), key());
  std::array<AudioReceiver *, 2> sources{&receiver, &good};
  std::array<float, 480> ordinary{};
  require(AudioReceiver::mix(sources, ordinary), "One failed source stopped another healthy source");
  require(good.status().state == EngineState::Running, "Failed source changed healthy source status");
}
void tenMillisecondLoopback()
{
  PacketBox box;
  AudioSender sender({48000, 2}, 10, config(480), key(), [&](auto packet) { return box.put(packet); });
  sender.start();
  std::array<float, 960> pcm{};
  const auto half = tone();
  std::copy(half.begin(), half.end(), pcm.begin());
  std::copy(half.begin(), half.end(), pcm.begin() + 480);
  require(sender.capture(pcm, false, monotonicNowNs()), "10ms capture failed");
  box.wait(1);
  sender.stop();
  AudioReceiver receiver({48000, 2}, 10, config(480), key());
  const auto now = monotonicNowNs();
  require(receiver.ingest(box.packets[0], now), "10ms authentication failed");
  receiver.tick(now + 5 * ms);
  require(receiver.stats().decodedPackets == 1, "10ms packet startup should still be 5ms, not capacity/duration");
  std::array<float, 960> output{};
  require(receiver.render(output), "10ms render failed");
}
void bandLimitedDeviceIntegration()
{
  PacketBox box;
  AudioSender sender({96000, 2}, 5, config(), key(), [&](auto packet) { return box.put(packet); });
  require(sender.adaptationLatencyInputFrames() == 64, "96k capture must use band-limited lookahead");
  sender.start();
  std::array<float, 960> input{};
  for (std::size_t frame = 0; frame < input.size() / 2; ++frame)
    input[frame * 2] = input[frame * 2 + 1] = .05f * std::sin(float(frame) * .2f);
  require(sender.capture(input, false, monotonicNowNs()), "96k first callback rejected");
  require(sender.capture(input, false, monotonicNowNs()), "96k second callback rejected");
  box.wait(1);
  sender.stop();
  AudioReceiver receiver({44100, 1}, 5, config(), key());
  require(receiver.adaptationLatencyMediaFrames() >= 32, "44.1k receiver must use FIR not linear baseline");
  const auto now = monotonicNowNs();
  require(receiver.ingest(box.packets.front(), now), "96k -> Opus -> 44.1k integration authentication failed");
  receiver.tick(now + 5 * ms);
  require(
      receiver.stats().playbackFrames > 0 && receiver.stats().playbackFrames < 221,
      "FIR lookahead should delay first output without padding media"
  );
  std::array<float, 221> output{};
  require(receiver.render(output), "Adapted mono device render failed");
}
void lateReorderedPacketCannotStopForwardMedia()
{
  DirectPackets packets;
  AudioReceiver receiver({48000, 2}, 5, config(), key());
  constexpr std::uint64_t start = 1000000000;
  auto delayed = packets.packet(0);
  std::array<float, 480> output{};
  for (std::uint16_t i = 1; i <= 20; ++i) {
    require(receiver.ingest(packets.packet(i), start + i * 5 * ms), "Forward stream rejected");
    receiver.tick(start + i * 5 * ms);
    (void)receiver.render(output);
  }
  require(!receiver.ingest(delayed, start + 101 * ms), "Delayed old packet should be jitter-rejected");
  require(!receiver.stats().requiresReanchor, "A single late reordered packet stopped healthy media");
  require(receiver.ingest(packets.packet(21), start + 105 * ms), "Forward stream did not survive old packet");
}
void longGapRequiresTrustedResume()
{
  DirectPackets packets;
  AudioReceiver receiver({48000, 2}, 5, config(), key());
  constexpr std::uint64_t start = 1000000000;
  require(receiver.ingest(packets.packet(0), start), "Initial gap-test packet rejected");
  std::array<float, 480> output{};
  // Keep decoder callbacks alive while the source has no WASAPI capture callbacks.
  for (unsigned i = 1; i <= 210; ++i) {
    receiver.tick(start + i * 5 * ms);
    (void)receiver.render(output);
  }
  require(receiver.stats().requiresReanchor, "No-callback silence gap must be surfaced");
  require(!receiver.ingest(packets.packet(1), start + 1100 * ms), "UDP must not auto-reset stale media timeline");
  receiver.reanchor(2, 480, start + 1200 * ms);
  (void)receiver.render(output); // Reader acknowledges stale PCM before resumed decode.
  require(receiver.ingest(packets.packet(2), start + 1200 * ms), "Trusted gap resume rejected");
  receiver.tick(start + 1205 * ms);
  require(!receiver.stats().requiresReanchor && receiver.stats().decodedPackets == 2, "Trusted gap resume failed");
}
void formatAndIndependentMix()
{
  AudioReceiver first({44100, 1}, 5, config(), key());
  AudioReceiver second({44100, 1}, 5, config(), key());
  first.setMuted(true);
  second.setMuted(false);
  require(first.outputFormat() == PcmFormat{44100, 1}, "Output format lost");
  require(first.muted() && !second.muted(), "Independent sources share mute state");
  std::array<float, 441> output{};
  std::array<AudioReceiver *, 2> sources{&first, &second};
  require(AudioReceiver::mix(sources, output), "Independent matching format mix failed");
  sources[1] = &first;
  require(!AudioReceiver::mix(sources, output), "Mix accepted same source twice");
}

// 真实 Opus/SRTP 加虚拟到达时钟：0..15ms 抖动会产生乱序和批交付，20ms 等待应容纳它们。
void fixedPrebufferSurvivesReorder()
{
  struct Delivery
  {
    std::uint64_t at;
    Bytes bytes;
  };
  DirectPackets packets;
  std::vector<Delivery> deliveries;
  constexpr std::uint64_t start = 1000000000;
  for (std::uint16_t sequence = 0; sequence < 200; ++sequence) {
    const auto jitter = sequence == 0 ? 0 : (sequence * 7U) % 16U;
    deliveries.push_back({start + (sequence * 5 + jitter) * ms, packets.packet(sequence)});
  }
  std::stable_sort(deliveries.begin(), deliveries.end(), [](const auto &a, const auto &b) { return a.at < b.at; });
  ReceiverOptions options;
  options.prebufferMs = 20;
  AudioReceiver receiver({48000, 2}, 5, config(), key(), options);
  std::array<float, 480> output{};
  std::size_t next = 0;
  // 到达 PLL 可轻微调整最后一包期限；留 4ms 余量但不越过下一包的名义播放周期。
  for (unsigned time = 0; time <= 1019; ++time) {
    const auto now = start + time * ms;
    while (next < deliveries.size() && deliveries[next].at <= now) {
      require(receiver.ingest(deliveries[next].bytes, now), "20ms prebuffer rejected bounded reordered delivery");
      ++next;
    }
    receiver.tick(now, now);
    if (time < 20)
      require(receiver.stats().decodedPackets == 0, "Media decoded before fixed prebuffer deadline");
    if (time % 5 == 0)
      (void)receiver.render(output, now);
  }
  const auto stats = receiver.stats();
  require(stats.decodedPackets == 200 && !stats.requiresReanchor, "Buffered stream did not finish normally");
  require(stats.jitterLatePackets == 0 && stats.jitterLostPackets == 0, "Bounded jitter caused media loss");
  require(
      stats.playbackFrames <= 1440 && stats.playbackRejectedFrames == 0,
      "Media prebuffer incorrectly accumulated decoded PCM"
  );
}

// 十二包队列不能用 16 位序号直接取模记录到达时间；跨 65535 回绕后，
// 仍应为每一包准确记录 20ms 驻留时间，避免长期运行的诊断数据被覆盖。
void fixedPrebufferAcrossSequenceWrap()
{
  DirectPackets packets;
  ReceiverOptions options;
  options.prebufferMs = 20;
  AudioReceiver receiver({48000, 2}, 5, config(), key(), options);
  constexpr std::uint64_t start = 1000000000;
  std::array<float, 480> output{};
  for (unsigned time = 0; time <= 115; time += 5) {
    const auto now = start + time * ms;
    if (time < 100) {
      const auto extended = 65530U + time / 5;
      require(
          receiver.ingest(packets.packet(static_cast<std::uint16_t>(extended), extended * 240), now),
          "Fixed prebuffer rejected sequence wrap"
      );
    }
    receiver.tick(now, now);
    (void)receiver.render(output, now);
  }
  const auto stats = receiver.stats();
  require(stats.decodedPackets == 20 && !stats.requiresReanchor, "Sequence wrap stopped buffered media");
  require(stats.jitterResidenceNsTotal == 20 * 20 * ms, "Sequence wrap overwrote buffered arrival statistics");
}

// 分别验证媒体偏移、线程停顿、无媒体三类原因；恢复保留原密钥、防重放及用户静音状态。
void authorizedRecoveryPreservesReplayProtection()
{
  constexpr std::uint64_t start = 1000000000;
  for (const auto reason :
       {RecoveryReason::MediaTimeline, RecoveryReason::DecoderStall, RecoveryReason::MediaTimeout}) {
    DirectPackets packets;
    ReceiverOptions options;
    options.prebufferMs = 20;
    AudioReceiver receiver({48000, 2}, 5, config(), key(), options);
    receiver.setMuted(true);
    auto first = packets.packet(0);
    require(receiver.ingest(first, start), "Recovery fixture initial authentication failed");
    receiver.tick(start + 20 * ms);
    std::array<float, 480> output{};
    (void)receiver.render(output, start + 20 * ms);
    auto beforeAuthorization = packets.packet(1);
    std::uint64_t resumeAt = start + 110 * ms;
    if (reason == RecoveryReason::MediaTimeline) {
      require(!receiver.ingest(beforeAuthorization, start + 100 * ms), "Large media phase jump was ignored");
    } else if (reason == RecoveryReason::DecoderStall) {
      receiver.tick(start + 100 * ms);
    } else {
      for (unsigned time = 25; time <= 1010; time += 5) {
        receiver.tick(start + time * ms);
        (void)receiver.render(output, start + time * ms);
      }
      resumeAt = start + 1100 * ms;
    }
    require(receiver.stats().recoveryReason == reason, "Recovery reason was misclassified");
    require(!receiver.ingest(beforeAuthorization, resumeAt), "UDP reset a timeline before trusted authorization");
    require(
        receiver.authorizeReanchor({1, 240}, receiver.stats().recoveryEpoch), "Current recovery authorization failed"
    );
    auto resumed = packets.packet(2);
    auto corrupt = resumed;
    corrupt.back() ^= 1;
    require(!receiver.ingest(corrupt, resumeAt), "Corrupt packet consumed recovery authorization");
    require(!receiver.ingest(first, resumeAt), "Reanchor cleared the SRTP replay window");
    require(receiver.ingest(resumed, resumeAt), "Authorized next media packet did not restore the timeline");
    (void)receiver.render(output, resumeAt); // reader 确认清空旧 PCM 后才能发布恢复后的声音。
    receiver.tick(resumeAt + 19 * ms);
    require(receiver.stats().decodedPackets == 1, "Recovery skipped the fixed prebuffer interval");
    receiver.tick(resumeAt + 20 * ms);
    const auto stats = receiver.stats();
    require(
        stats.decodedPackets == 2 && !stats.requiresReanchor && receiver.muted(),
        "Recovery lost decoder progress or changed user mute"
    );
    require(
        stats.timelineRecoveries + stats.schedulerRecoveries + stats.timeoutRecoveries == 1,
        "One recovery episode was counted repeatedly"
    );
    require(!receiver.ingest(resumed, resumeAt + 21 * ms), "Recovered packet could be replayed");
  }
}
// 模拟可信回复早于首包，以及同一批 UDP 内刚重锚又发生偏移；旧轮次不能代替新授权。
void recoveryAuthorizationTracksEpoch()
{
  constexpr std::uint64_t start = 1000000000;
  DirectPackets packets;
  AudioReceiver receiver({48000, 2}, 5, config(), key());
  require(!receiver.authorizeReanchor({0, 0}, 0), "Initial progress was incorrectly marked as an active authorization");
  require(receiver.ingest(packets.packet(1), start), "Initial packet did not establish its own timeline");
  require(!receiver.ingest(packets.packet(21), start), "Batched timeline jump did not request recovery");
  const auto firstEpoch = receiver.stats().recoveryEpoch;
  require(firstEpoch == 1, "First recovery did not advance its epoch");
  require(!receiver.authorizeReanchor({21, 21 * 240}, 0), "Initial progress reply authorized a later fault");
  require(receiver.authorizeReanchor({21, 21 * 240}, firstEpoch), "First active recovery rejected trusted progress");
  const auto resumed = packets.packet(22);
  require(receiver.ingest(resumed, start), "Authorized media did not reanchor");
  // 不插入 tick 或 Session 状态检查，直接交付第二个偏移包，复现一次排空内遗漏恢复完成。
  require(!receiver.ingest(packets.packet(42), start), "Immediate second timeline jump was ignored");
  const auto secondEpoch = receiver.stats().recoveryEpoch;
  require(secondEpoch == firstEpoch + 1, "Second fault reused the consumed recovery epoch");
  require(!receiver.authorizeReanchor({42, 42 * 240}, firstEpoch), "Stale epoch authorized a different recovery");
  require(!receiver.ingest(packets.packet(43), start), "Second fault retained the first authorization");
  require(receiver.authorizeReanchor({43, 43 * 240}, secondEpoch), "New epoch could not be authorized again");
  require(receiver.ingest(packets.packet(44), start), "New trusted progress did not restore media");
  require(!receiver.stats().requiresReanchor, "Second recovery remained stuck awaiting authorization");
  require(!receiver.authorizeReanchor({44, 44 * 240}, secondEpoch), "Completed recovery still accepted authorization");
  require(!receiver.ingest(resumed, start), "Recovery epoch reset the SRTP replay window");
}
} // namespace
int main()
{
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  try {
    asyncAuthenticatedLoopback();
    boundedPlcAndMute();
    lossFreezesPlaybackFeedback();
    callbackBetweenDecodeAndObservationMakesProgress();
    overloadBoundedAndMediaTimeline();
    exactZeroGateSignals();
    workerExceptionAndGateContract();
    trustedReanchorFlushAndPlaybackCap();
    tenMillisecondLoopback();
    unsupportedPlaybackQuantumFailsClearly();
    formatAndIndependentMix();
    longGapRequiresTrustedResume();
    lateReorderedPacketCannotStopForwardMedia();
    bandLimitedDeviceIntegration();
    fixedPrebufferSurvivesReorder();
    fixedPrebufferAcrossSequenceWrap();
    authorizedRecoveryPreservesReplayProtection();
    recoveryAuthorizationTracksEpoch();
    std::cout << "Engine tests passed: worker/auth/5ms-startup/PLC/mute/overload/timeline/format/mix\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Engine tests failed: " << error.what() << '\n';
    return 1;
  }
}
