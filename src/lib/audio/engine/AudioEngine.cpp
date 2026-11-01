/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "AudioEngine.h"

#include "../codec/OpusCodec.h"
#include "../dsp/CapturePipeline.h"
#include "../dsp/ClockDrift.h"
#include "../dsp/JitterBuffer.h"
#include "../dsp/NetworkClock.h"
#include "../dsp/Playback.h"
#include "../dsp/PlaybackClock.h"
#include "../dsp/SilenceGate.h"
#include "../dsp/StreamAdapter.h"
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../windows/WorkerTimer.h"
#endif

namespace deskflow::audio::engine {
namespace {
constexpr PcmFormat kMediaFormat{48000, 2};
constexpr std::uint64_t kMillisecond = 1000000;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);

StreamConfig checkedConfig(std::uint32_t ms, StreamConfig config)
{
  if ((ms != 5 && ms != 10) || !config.valid() || config.frameSamples != ms * 48) {
    throw std::invalid_argument("Engine requires matching 5/10ms, 48kHz stereo stream configuration");
  }
  return config;
}
std::uint64_t addSaturated(std::uint64_t a, std::uint64_t b) noexcept
{
  return a > std::numeric_limits<std::uint64_t>::max() - b ? std::numeric_limits<std::uint64_t>::max() : a + b;
}
void recordTiming(std::atomic<std::uint64_t> &total, std::atomic<std::uint64_t> &maximum, std::uint64_t value) noexcept
{
  total.fetch_add(value, std::memory_order_relaxed);
  // Each timing pair has exactly one worker writer; readers only load snapshots.
  if (value > maximum.load(std::memory_order_relaxed)) {
    maximum.store(value, std::memory_order_relaxed);
  }
}
class WorkerIdleWait final
{
public:
  WorkerIdleWait() noexcept
  {
#ifdef _WIN32
    highResolution = m_timer.highResolution();
    coarseFallback = !highResolution;
#endif
  }
  void wait() noexcept
  {
#ifdef _WIN32
    m_timer.waitMicroseconds(250);
#else
    std::this_thread::sleep_for(std::chrono::microseconds(250));
#endif
  }
  bool highResolution = false, coarseFallback = false;

private:
#ifdef _WIN32
  windows::WorkerTimer m_timer;
#endif
};
struct StatusAtoms
{
  std::atomic<EngineState> state{EngineState::Ready};
  std::atomic<EngineError> error{EngineError::None};
  void fail(EngineError value) noexcept
  {
    error.store(value, std::memory_order_relaxed);
    state.store(EngineState::Failed, std::memory_order_release);
  }
  EngineStatus get() const noexcept
  {
    const auto current = state.load(std::memory_order_acquire);
    return {current, error.load(std::memory_order_relaxed)};
  }
};
} // namespace

std::uint64_t monotonicNowNs() noexcept
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
  );
}

struct AudioSender::Impl
{
  const PcmFormat input;
  const StreamConfig config;
  const std::uint64_t maxAgeNs;
  dsp::CapturePipeline capture;
  codec::OpusCodec codec;
  transport::SrtpSender transport;
  dsp::SilenceGate gate{kMediaFormat, 48000 / 5}; // 200ms of EXACT zeros, never a level/VAD threshold.
  PacketSink sink;
  SenderOptions options;
  StatusAtoms status;
  std::atomic<bool> stopRequested{false};
  std::thread worker;
  std::atomic<std::uint64_t> expired{0}, sent{0}, sinkDropped{0}, suppressed{0}, encoded{0};
  std::atomic<std::size_t> queued{0};
  std::atomic<std::uint64_t> timingSamples{0}, residenceTotal{0}, residenceMax{0}, encodeTotal{0}, encodeMax{0};
  std::atomic<bool> highResolutionWait{false}, coarseWaitFallback{false};

  Impl(PcmFormat in, std::uint32_t ms, StreamConfig cfg, SrtpKey &&key, PacketSink output, SenderOptions opts)
      : input(in),
        config(checkedConfig(ms, cfg)),
        maxAgeNs(ms * 2 * kMillisecond),
        capture(in, kMediaFormat, ms, 4, config.generation, 0, in.sampleRate != kMediaFormat.sampleRate),
        codec(config.frameSamples, opts.bitrate),
        transport(config, std::move(key)),
        sink(std::move(output)),
        options(std::move(opts))
  {
    if (!sink || (options.exactZeroGate && !options.silenceSignal)) {
      throw std::invalid_argument("Sender needs nonblocking packet sink and explicit signalling for silence gate");
    }
  }
  void run() noexcept
  {
    try {
      WorkerIdleWait idle;
      highResolutionWait.store(idle.highResolution, std::memory_order_relaxed);
      coarseWaitFallback.store(idle.coarseFallback, std::memory_order_relaxed);
      std::array<float, 960> pcm{};
      std::array<std::uint8_t, transport::kMaxPayloadBytes> opus{};
      bool haveProtected = false;
      std::uint64_t lastProtected = 0;
      while (!stopRequested.load(std::memory_order_acquire)) {
        dsp::PcmBlockInfo info;
        auto &queue = capture.queue();
        queued.store(queue.queuedBlocks(), std::memory_order_relaxed);
        if (!queue.peek(info)) {
          // No callback-side wake/lock/syscall. A bounded idle poll keeps capture realtime.
          idle.wait();
          continue;
        }
        const auto now = monotonicNowNs();
        const bool wallExpired = info.enqueuedNs != 0 && now >= info.enqueuedNs && now - info.enqueuedNs > maxAgeNs;
        const auto latest = capture.latestTimestampFrames();
        const bool mediaExpired =
            latest >= info.timestampFrames && latest - info.timestampFrames > std::uint64_t(config.frameSamples) * 2;
        if (wallExpired || mediaExpired) {
          (void)queue.discard();
          expired.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (!queue.pop(pcm, info)) {
          continue;
        }
        const auto samples = std::span<float>(pcm.data(), std::size_t(config.frameSamples) * 2);
        // CapturePipeline consumes a sequence for EVERY full media block, even a full-queue
        // rejection. RTP uses that media-slot sequence (not sent-packet count). Partial
        // callback discontinuities never introduce a fractional RTP timestamp increment.
        const auto sequence = static_cast<std::uint16_t>(info.sequence);
        const auto timestamp = static_cast<std::uint32_t>(info.sequence * config.frameSamples);
        if (options.exactZeroGate) {
          const auto decision = gate.inspect(samples);
          if (decision.enteredSilence || decision.resumed) {
            options.silenceSignal(
                {decision.resumed ? SilenceEvent::Resumed : SilenceEvent::Entered, config.generation, sequence,
                 timestamp}
            );
          }
          if (!decision.send) {
            suppressed.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          if (decision.resumed) {
            codec.resetEncoder();
          }
        }
        // libSRTP derives ROC from the 16-bit sequence. Do not silently cross an ambiguous
        // half-sequence-space gap, including before the first protected packet. A fresh
        // trusted key+SSRC/new object is required, never reset this context's packet index.
        if ((!haveProtected && info.sequence >= 32768) ||
            (haveProtected && (info.sequence <= lastProtected || info.sequence - lastProtected >= 32768)) ||
            info.sequence >= (std::uint64_t(1) << 48)) {
          status.fail(EngineError::SequenceExhausted);
          return;
        }
        if (info.discontinuity) {
          codec.resetEncoder();
        }
        for (auto &sample : samples) {
          sample = dsp::finiteSample(sample);
        }
        const auto encodeStart = monotonicNowNs();
        const auto bytes = codec.encode(samples, opus);
        recordTiming(encodeTotal, encodeMax, monotonicNowNs() - encodeStart);
        if (info.enqueuedNs != 0 && encodeStart >= info.enqueuedNs) {
          recordTiming(residenceTotal, residenceMax, encodeStart - info.enqueuedNs);
        }
        timingSamples.fetch_add(1, std::memory_order_relaxed);
        if (bytes <= 0) {
          status.fail(EngineError::CodecFailure);
          return;
        }
        encoded.fetch_add(1, std::memory_order_relaxed);
        const auto packet = transport.protect(std::span<const std::uint8_t>(opus.data(), bytes), sequence, timestamp);
        if (!packet) {
          status.fail(EngineError::CryptoFailure);
          return;
        }
        haveProtected = true;
        lastProtected = info.sequence; // Even sink rejection has irrevocably consumed this index.
        if (sink(packet.packet)) {
          sent.fetch_add(1, std::memory_order_relaxed);
        } else {
          sinkDropped.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } catch (...) {
      status.fail(EngineError::WorkerException); // Never allow a callback exception to std::terminate.
    }
  }
};

AudioSender::AudioSender(
    PcmFormat input, std::uint32_t packetMs, StreamConfig config, SrtpKey &&key, PacketSink sink, SenderOptions options
)
    : m_impl(std::make_unique<Impl>(input, packetMs, config, std::move(key), std::move(sink), std::move(options)))
{
}
AudioSender::~AudioSender()
{
  stop();
}
void AudioSender::start()
{
  auto expected = EngineState::Ready;
  if (!m_impl->status.state.compare_exchange_strong(expected, EngineState::Running, std::memory_order_acq_rel)) {
    throw std::logic_error("AudioSender start is one-shot");
  }
  try {
    m_impl->worker = std::thread([impl = m_impl.get()] { impl->run(); });
  } catch (...) {
    m_impl->status.fail(EngineError::WorkerException);
    throw;
  }
}
void AudioSender::stop() noexcept
{
  m_impl->stopRequested.store(true, std::memory_order_release);
  // Control-thread API. Calling stop() from the sink itself would join this worker.
  if (m_impl->worker.joinable() && m_impl->worker.get_id() != std::this_thread::get_id()) {
    m_impl->worker.join();
  }
  if (m_impl->status.state.load(std::memory_order_acquire) != EngineState::Failed) {
    m_impl->status.state.store(EngineState::Stopped, std::memory_order_release);
  }
}
bool AudioSender::capture(std::span<const float> pcm, bool discontinuity, std::uint64_t nowNs) noexcept
{
  return m_impl->status.state.load(std::memory_order_acquire) == EngineState::Running &&
         m_impl->capture.push(pcm, discontinuity, nowNs);
}
PcmFormat AudioSender::inputFormat() const noexcept
{
  return m_impl->input;
}
std::size_t AudioSender::adaptationLatencyInputFrames() const noexcept
{
  return m_impl->capture.latencyInputFrames();
}
EngineStatus AudioSender::status() const noexcept
{
  return m_impl->status.get();
}
SenderStats AudioSender::stats() const noexcept
{
  const auto capture = m_impl->capture.stats();
  return {
      capture.producedBlocks,
      capture.droppedBlocks,
      m_impl->expired.load(std::memory_order_relaxed),
      m_impl->sent.load(std::memory_order_relaxed),
      m_impl->sinkDropped.load(std::memory_order_relaxed),
      m_impl->suppressed.load(std::memory_order_relaxed),
      m_impl->encoded.load(std::memory_order_relaxed),
      m_impl->queued.load(std::memory_order_relaxed),
      m_impl->timingSamples.load(std::memory_order_relaxed),
      m_impl->residenceTotal.load(std::memory_order_relaxed),
      m_impl->residenceMax.load(std::memory_order_relaxed),
      m_impl->encodeTotal.load(std::memory_order_relaxed),
      m_impl->encodeMax.load(std::memory_order_relaxed),
      m_impl->highResolutionWait.load(std::memory_order_relaxed),
      m_impl->coarseWaitFallback.load(std::memory_order_relaxed)
  };
}

struct AudioReceiver::Impl
{
  const PcmFormat device;
  const StreamConfig config;
  const std::uint64_t durationNs;
  const ReceiverOptions options;
  codec::OpusCodec codec;
  transport::SrtpReceiver transport;
  dsp::JitterBuffer jitter;
  dsp::NetworkClock networkClock;
  dsp::BandLimitedAdapter adapter;
  dsp::PlaybackSource playback;
  dsp::ClockDriftController drift;
  dsp::PlaybackClock playbackClock;
  std::uint64_t lastDriftUpdateNs = 0, driftResumeNs = 0;
  bool rejectedPcmSinceTick = false;
  StatusAtoms status;
  std::array<float, 960> pcm{};
  // Max 10ms at 192kHz stereo plus interpolation/correction margin.
  std::array<float, 4096> adapted{};
  std::array<std::uint8_t, transport::kMaxPayloadBytes> opus{};
  bool started = false, outputBreak = true;
  unsigned consecutiveLosses = 0;
  std::uint64_t lastTimestamp = 0, nextDeadline = 0;
  std::array<std::uint64_t, 8> arrivalNs{};
  std::atomic<bool> needsReanchor{false};
  std::atomic<std::uint64_t> jitterLost{0}, jitterLate{0}, timingSamples{0};
  std::atomic<std::uint64_t> decodeTotal{0}, decodeMax{0}, residenceTotal{0}, residenceMax{0}, reanchorDiscarded{0};
  std::atomic<bool> flushReader{false};
  std::atomic<std::uint64_t> authenticated{0}, rejected{0}, jitterRejected{0}, decoded{0};
  std::atomic<std::uint64_t> plc{0}, zeros{0}, decodeErrors{0};
  std::atomic<std::size_t> jitterPackets{0}, playbackFrames{0};
  std::atomic<double> correction{0}, networkCorrection{0};
  std::atomic<std::uint64_t> feedbackUpdates{0}, feedbackConflicts{0}, feedbackInvalid{0}, feedbackCooldown{0};
  std::uint64_t lastArrivalNs = 0;
  bool haveForwardSequence = false;
  std::uint16_t lastForwardSequence = 0;

  Impl(PcmFormat output, std::uint32_t ms, StreamConfig cfg, SrtpKey &&key, ReceiverOptions opts)
      : device(output),
        config(checkedConfig(ms, cfg)),
        durationNs(ms * kMillisecond),
        options(opts),
        codec(config.frameSamples),
        transport(config, std::move(key)),
        jitter(8, transport::kMaxPayloadBytes, config.frameSamples, durationNs),
        networkClock(durationNs),
        adapter(kMediaFormat, device),
        playback(
            device, device.framesForDuration(30),
            std::max(std::size_t(1), (std::size_t(device.sampleRate) * 5 + 999) / 1000), opts.initialVolume
        ),
        drift(
            dsp::DriftConfig{
                double(device.sampleRate) * 0.012, 3.0 * 48000.0 / device.sampleRate, 0.1 * 48000.0 / device.sampleRate
            }
        )
  {
    status.state.store(EngineState::Running, std::memory_order_release);
  }
  void anchor(std::uint16_t sequence, std::uint32_t timestamp, std::uint64_t nowNs, bool flush) noexcept
  {
    nextDeadline = addSaturated(nowNs, 5 * kMillisecond);
    jitter.start(config.generation, sequence, timestamp, nextDeadline);
    networkClock.reset();
    networkCorrection.store(0, std::memory_order_relaxed);
    lastArrivalNs = nowNs;
    haveForwardSequence = false;
    arrivalNs.fill(0);
    needsReanchor.store(false, std::memory_order_relaxed);
    started = true;
    lastTimestamp = timestamp;
    consecutiveLosses = 0;
    codec.resetDecoder();
    adapter.reset();
    drift.reset();
    lastDriftUpdateNs = 0;
    correction.store(0, std::memory_order_relaxed);
    outputBreak = true;
    jitterPackets.store(0, std::memory_order_relaxed);
    if (flush) {
      // Only the device reader can move its read index. Never reset PlaybackSource
      // on this writer thread; volume/mute and SPSC ownership remain intact.
      flushReader.store(true, std::memory_order_release);
    }
  }
  bool prepareReader() noexcept
  {
    bool discontinuity = false;
    if (flushReader.load(std::memory_order_acquire)) {
      (void)playback.trimTo(0);
      flushReader.store(false, std::memory_order_release);
      discontinuity = true;
    }
    const auto trimFrames = (std::size_t(device.sampleRate) * 25 + 999) / 1000;
    if (playback.queuedFrames() > trimFrames) {
      // 12ms normalized pre-write target + <=10ms packet leaves burst headroom.
      (void)playback.trimTo(trimFrames);
      discontinuity = true;
    }
    return discontinuity;
  }
  void publish() noexcept
  {
    jitterPackets.store(jitter.queuedPackets(), std::memory_order_relaxed);
    const auto jitterStats = jitter.stats();
    jitterLost.store(jitterStats.lost, std::memory_order_relaxed);
    jitterLate.store(jitterStats.late, std::memory_order_relaxed);
    playbackFrames.store(playback.queuedFramesForWriter(), std::memory_order_relaxed);
  }
  void updateDrift(std::uint64_t nowNs) noexcept
  {
    if (nowNs < driftResumeNs) {
      feedbackCooldown.fetch_add(1, std::memory_order_relaxed);
      lastDriftUpdateNs = 0;
      return;
    }
    double totalFrames = 0;
    bool transientConflict = false;
    if (!playbackClock.observe(
            nowNs, device.sampleRate, [&] { return playback.queuedFramesForWriter(); }, totalFrames, &transientConflict
        )) {
      // A short conflicting callback skips evidence, not the established time base.
      // The next valid observation can integrate its bounded elapsed interval.
      if (!transientConflict) {
        feedbackInvalid.fetch_add(1, std::memory_order_relaxed);
        lastDriftUpdateNs = 0;
      } else {
        feedbackConflicts.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    if (lastDriftUpdateNs == 0) {
      lastDriftUpdateNs = nowNs;
      return;
    }
    if (nowNs <= lastDriftUpdateNs)
      return; // Coalesced decoder work is not another time step.
    const auto elapsed = nowNs - lastDriftUpdateNs;
    if (elapsed < durationNs / 2)
      return;
    lastDriftUpdateNs = nowNs;
    if (elapsed > 2 * durationNs)
      return; // Scheduling gaps are not frequency evidence.
    const auto ppm = drift.update(totalFrames, double(elapsed) / 1e9);
    feedbackUpdates.fetch_add(1, std::memory_order_relaxed);
    correction.store(ppm, std::memory_order_relaxed);
    (void)adapter.setRateCorrectionPpm(ppm);
  }
  void writePcm(bool discontinuity) noexcept
  {
    if (flushReader.load(std::memory_order_acquire)) {
      // Wait for reader-owned stale-data flush acknowledgement. Never publish new
      // PCM into a ring which the reader is about to clear after a trusted anchor.
      outputBreak = true;
      reanchorDiscarded.fetch_add(config.frameSamples, std::memory_order_relaxed);
      return;
    }
    auto input = std::span<const float>(pcm.data(), std::size_t(config.frameSamples) * 2);
    bool first = true;
    for (;;) {
      const auto progress = adapter.process(input, adapted);
      input = input.subspan(progress.inputFrames * 2);
      if (progress.outputFrames != 0) {
        const auto block = std::span<const float>(adapted.data(), progress.outputFrames * device.channels);
        if (!playback.write(block, first && discontinuity, monotonicNowNs())) {
          rejectedPcmSinceTick = true;
        }
        first = false;
      }
      if (progress.inputFrames == 0 && progress.outputFrames == 0) {
        break;
      }
    }
  }
};

AudioReceiver::AudioReceiver(
    PcmFormat device, std::uint32_t packetMs, StreamConfig config, SrtpKey &&key, ReceiverOptions options
)
    : m_impl(std::make_unique<Impl>(device, packetMs, config, std::move(key), options))
{
}
AudioReceiver::~AudioReceiver() = default;
bool AudioReceiver::ingest(std::span<const std::uint8_t> datagram, std::uint64_t nowNs) noexcept
{
  if (m_impl->status.state.load(std::memory_order_acquire) != EngineState::Running) {
    return false;
  }
  const auto received = m_impl->transport.unprotect(datagram);
  if (!received) {
    m_impl->rejected.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  m_impl->authenticated.fetch_add(1, std::memory_order_relaxed);
  if (!m_impl->started) {
    // This binds only the initial media timeline, NEVER config/key/SSRC/generation.
    m_impl->anchor(received.metadata.sequence, received.metadata.timestamp, nowNs, false);
  }
  if (m_impl->needsReanchor.load(std::memory_order_relaxed))
    return false;
  const auto mappedDeadline = m_impl->jitter.deadlineFor(received.metadata.sequence);
  const auto desiredDeadline = addSaturated(nowNs, 5 * kMillisecond);
  const auto phaseError =
      desiredDeadline >= mappedDeadline ? desiredDeadline - mappedDeadline : mappedDeadline - desiredDeadline;
  const auto forwardDelta = static_cast<std::uint16_t>(received.metadata.sequence - m_impl->lastForwardSequence);
  const bool forward = !m_impl->haveForwardSequence || (forwardDelta != 0 && forwardDelta < 32768);
  if (forward && phaseError > 40 * kMillisecond) {
    m_impl->jitter.stop();
    m_impl->needsReanchor.store(true, std::memory_order_relaxed);
    m_impl->flushReader.store(true, std::memory_order_release);
    m_impl->publish();
    return false; // Only the trusted control plane can recover a discontinuous media clock.
  }
  if (forward) {
    m_impl->haveForwardSequence = true;
    m_impl->lastForwardSequence = received.metadata.sequence;
    const auto period = m_impl->networkClock.observe(received.metadata.sequence, nowNs, mappedDeadline);
    (void)m_impl->jitter.setPacketDurationNs(period);
    m_impl->networkCorrection.store(m_impl->networkClock.correctionPpm(), std::memory_order_relaxed);
    m_impl->lastArrivalNs = nowNs;
  }
  const auto delta =
      static_cast<std::int32_t>(received.metadata.timestamp - static_cast<std::uint32_t>(m_impl->lastTimestamp));
  const auto timestamp = m_impl->lastTimestamp + static_cast<std::uint64_t>(static_cast<std::int64_t>(delta));
  const dsp::PacketInfo info{received.metadata.sequence, timestamp, m_impl->config.generation, false};
  const auto inserted = m_impl->jitter.insert(info, received.payload, nowNs);
  if (inserted != dsp::PacketInsert::Accepted) {
    m_impl->jitterRejected.fetch_add(1, std::memory_order_relaxed);
    m_impl->publish();
    return false;
  }
  m_impl->arrivalNs[received.metadata.sequence % 8] = nowNs;
  if (delta > 0) {
    m_impl->lastTimestamp = timestamp;
  }
  m_impl->publish();
  return true;
}
void AudioReceiver::tick(std::uint64_t nowNs, std::uint64_t feedbackNs) noexcept
{
  if (m_impl->status.state.load(std::memory_order_acquire) != EngineState::Running) {
    return;
  }
  if (m_impl->started && ((nowNs > m_impl->nextDeadline && nowNs - m_impl->nextDeadline > m_impl->durationNs * 8) ||
                          (nowNs > m_impl->lastArrivalNs && nowNs - m_impl->lastArrivalNs > 1000 * kMillisecond))) {
    // A severe scheduler stall is NOT permission to reanchor from UDP. Stop media
    // decisions and request an explicit trusted reanchor instead of chasing stale PLC.
    m_impl->jitter.stop();
    m_impl->needsReanchor.store(true, std::memory_order_relaxed);
    m_impl->flushReader.store(true, std::memory_order_release);
    m_impl->publish();
    return;
  }
  if (m_impl->needsReanchor.load(std::memory_order_relaxed)) {
    return;
  }
  // Bound scheduler catch-up independently of jitter capacity. Never generate an
  // unbounded PLC tail or spin for arbitrarily long in a delayed decoder tick.
  bool driftObserved = false;
  for (unsigned work = 0; work < 8; ++work) {
    const auto packet = m_impl->jitter.poll(nowNs, m_impl->opus);
    if (packet.status != dsp::PacketPoll::Packet && packet.status != dsp::PacketPoll::Loss) {
      break;
    }
    m_impl->nextDeadline = addSaturated(packet.deadlineNs, m_impl->networkClock.packetDurationNs());
    bool haveAudio = false;
    bool discontinuity = m_impl->outputBreak;
    if (packet.status == dsp::PacketPoll::Packet) {
      const auto decodeStart = monotonicNowNs();
      const auto frames =
          m_impl->codec.decode(std::span<const std::uint8_t>(m_impl->opus.data(), packet.bytes), m_impl->pcm);
      recordTiming(m_impl->decodeTotal, m_impl->decodeMax, monotonicNowNs() - decodeStart);
      const auto arrival = m_impl->arrivalNs[packet.info.sequence % 8];
      if (nowNs >= arrival) {
        recordTiming(m_impl->residenceTotal, m_impl->residenceMax, nowNs - arrival);
      }
      m_impl->timingSamples.fetch_add(1, std::memory_order_relaxed);
      if (frames == m_impl->config.frameSamples) {
        haveAudio = true;
        discontinuity = discontinuity || m_impl->consecutiveLosses != 0;
        m_impl->consecutiveLosses = 0;
        m_impl->decoded.fetch_add(1, std::memory_order_relaxed);
      } else {
        m_impl->decodeErrors.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (!haveAudio) {
      const bool exhaustedNow = m_impl->consecutiveLosses == 2;
      m_impl->consecutiveLosses = std::min(m_impl->consecutiveLosses + 1, 3U);
      if (m_impl->consecutiveLosses <= 2 && m_impl->decoded.load(std::memory_order_relaxed) != 0 &&
          m_impl->codec.decode({}, m_impl->pcm) == m_impl->config.frameSamples) {
        m_impl->plc.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::fill(m_impl->pcm.begin(), m_impl->pcm.end(), 0.0f);
        m_impl->zeros.fetch_add(1, std::memory_order_relaxed);
        if (exhaustedNow) {
          // Decoder state is not reused after a long empty tail. The next real
          // packet will decode fresh; no repeated last-frame implementation.
          m_impl->codec.resetDecoder();
          m_impl->drift.reset();
          m_impl->adapter.reset();
        }
        discontinuity = discontinuity || exhaustedNow;
      }
    }
    if (!haveAudio || discontinuity) {
      m_impl->lastDriftUpdateNs = 0;
      m_impl->driftResumeNs = addSaturated(nowNs, 30 * kMillisecond);
    }
    // A suspended worker must not enqueue a catch-up burst of expired decoded PCM.
    if (nowNs >= packet.deadlineNs && nowNs - packet.deadlineNs <= 10 * kMillisecond) {
      if (!driftObserved && work == 0 && haveAudio && !m_impl->flushReader.load(std::memory_order_acquire)) {
        if (nowNs - packet.deadlineNs < m_impl->durationNs / 2) {
          // Entry time can precede a callback that completed during decoding. Read
          // observation time here, not before decode; otherwise stable phase can starve PI.
          const auto observedNs =
              feedbackNs != 0
                  ? feedbackNs
                  : (m_impl->options.feedbackClock ? m_impl->options.feedbackClock(m_impl->options.feedbackClockContext)
                                                   : monotonicNowNs());
          m_impl->updateDrift(observedNs);
        } else {
          // A catch-up burst has missing-yet-to-be-written PCM. Its pre-write water
          // level is not comparable to a normally scheduled packet boundary.
          m_impl->lastDriftUpdateNs = 0;
        }
        driftObserved = true;
      }
      m_impl->writePcm(discontinuity);
      if (m_impl->rejectedPcmSinceTick) {
        m_impl->lastDriftUpdateNs = 0;
        m_impl->driftResumeNs = addSaturated(nowNs, 30 * kMillisecond);
        m_impl->rejectedPcmSinceTick = false;
      }
      m_impl->outputBreak = false;
    } else {
      m_impl->outputBreak = true;
    }
  }
  m_impl->publish();
}
void AudioReceiver::reanchor(std::uint16_t sequence, std::uint32_t timestamp, std::uint64_t nowNs) noexcept
{
  m_impl->anchor(sequence, timestamp, nowNs, true);
}
bool AudioReceiver::render(std::span<float> output, std::uint64_t callbackNs) noexcept
{
  if (output.empty())
    return true; // No callback quantum, no clock/flush side effects.
  if (!m_impl->device.wholeFrames(output)) {
    std::fill(output.begin(), output.end(), 0.0f);
    return false;
  }
  if (m_impl->status.state.load(std::memory_order_acquire) != EngineState::Running) {
    std::fill(output.begin(), output.end(), 0.0f);
    return false;
  }
  if (output.size() / m_impl->device.channels > (std::size_t(m_impl->device.sampleRate) + 99) / 100) {
    // The fixed 30ms ring / 25ms trim / 12ms normalized pre-write model is supported only
    // for callbacks <=10ms. Do not mask incompatible granularity with constant trims.
    m_impl->status.fail(EngineError::UnsupportedPlaybackQuantum);
    std::fill(output.begin(), output.end(), 0.0f);
    return false;
  }
  const auto nowNs = callbackNs != 0 ? callbackNs : monotonicNowNs();
  m_impl->playbackClock.begin();
  const bool discontinuity = m_impl->prepareReader();
  const auto before = m_impl->playback.stats().underrunFrames;
  const auto result = m_impl->playback.render(output, nowNs);
  const auto missing = m_impl->playback.stats().underrunFrames - before;
  m_impl->playbackClock.end(
      missing == 0 && !discontinuity ? nowNs : 0,
      output.size() / m_impl->device.channels - static_cast<std::size_t>(missing)
  );
  return result;
}
bool AudioReceiver::mix(std::span<AudioReceiver *const> sources, std::span<float> output, float ceiling) noexcept
{
  if (output.empty())
    return true; // Do not erase the preceding nonempty callback's pending PCM.
  if (sources.empty()) {
    std::fill(output.begin(), output.end(), 0.0f);
    return true;
  }
  if (sources.size() > 8 || sources.front() == nullptr) {
    std::fill(output.begin(), output.end(), 0.0f);
    return false;
  }
  const auto format = sources.front()->outputFormat();
  std::array<dsp::PlaybackSource *, 8> playback{};
  for (std::size_t i = 0; i < sources.size(); ++i) {
    if (sources[i] == nullptr || sources[i]->outputFormat() != format) {
      std::fill(output.begin(), output.end(), 0.0f);
      return false;
    }
    // Duplicate sources would consume a source twice and violate source independence.
    for (std::size_t j = 0; j < i; ++j) {
      if (sources[j] == sources[i]) {
        std::fill(output.begin(), output.end(), 0.0f);
        return false;
      }
    }
    playback[i] = &sources[i]->m_impl->playback;
  }
  if (!format.wholeFrames(output) || !std::isfinite(ceiling) || ceiling <= 0 || ceiling > 1) {
    std::fill(output.begin(), output.end(), 0.0f);
    return false;
  }
  if (output.size() / format.channels > (std::size_t(format.sampleRate) + 99) / 100) {
    for (auto *source : sources)
      source->m_impl->status.fail(EngineError::UnsupportedPlaybackQuantum);
    std::fill(output.begin(), output.end(), 0.0f);
    return false;
  }
  const auto nowNs = monotonicNowNs();
  std::array<AudioReceiver *, 8> active{};
  std::array<std::uint64_t, 8> underruns{};
  std::array<bool, 8> discontinuities{};
  std::size_t running = 0;
  for (auto *source : sources) {
    if (source->m_impl->status.state.load(std::memory_order_acquire) == EngineState::Running) {
      source->m_impl->playbackClock.begin();
      discontinuities[running] = source->m_impl->prepareReader();
      underruns[running] = source->m_impl->playback.stats().underrunFrames;
      active[running] = source;
      playback[running++] = &source->m_impl->playback;
    }
    // One failed source must not stop another source or consume stale failed PCM.
  }
  const auto result =
      dsp::mixSources(std::span<dsp::PlaybackSource *const>(playback.data(), running), format, output, ceiling, nowNs);
  for (std::size_t i = 0; i < running; ++i) {
    const auto missing = active[i]->m_impl->playback.stats().underrunFrames - underruns[i];
    active[i]->m_impl->playbackClock.end(
        missing == 0 && !discontinuities[i] ? nowNs : 0,
        output.size() / format.channels - static_cast<std::size_t>(missing)
    );
  }
  return result;
}
void AudioReceiver::setVolume(float value) noexcept
{
  m_impl->playback.setVolume(value);
}
void AudioReceiver::setMuted(bool value) noexcept
{
  m_impl->playback.setMuted(value);
}
bool AudioReceiver::muted() const noexcept
{
  return m_impl->playback.muted();
}
PcmFormat AudioReceiver::outputFormat() const noexcept
{
  return m_impl->device;
}
std::size_t AudioReceiver::adaptationLatencyMediaFrames() const noexcept
{
  return m_impl->adapter.latencyInputFrames();
}
EngineStatus AudioReceiver::status() const noexcept
{
  return m_impl->status.get();
}
ReceiverStats AudioReceiver::stats() const noexcept
{
  const auto playback = m_impl->playback.stats();
  return {
      m_impl->authenticated.load(std::memory_order_relaxed),
      m_impl->rejected.load(std::memory_order_relaxed),
      m_impl->jitterRejected.load(std::memory_order_relaxed),
      m_impl->decoded.load(std::memory_order_relaxed),
      m_impl->plc.load(std::memory_order_relaxed),
      m_impl->zeros.load(std::memory_order_relaxed),
      m_impl->decodeErrors.load(std::memory_order_relaxed),
      playback.rejectedFrames,
      playback.trimmedFrames,
      playback.underrunFrames,
      m_impl->jitterPackets.load(std::memory_order_relaxed),
      m_impl->playbackFrames.load(std::memory_order_relaxed),
      m_impl->correction.load(std::memory_order_relaxed),
      m_impl->jitterLost.load(std::memory_order_relaxed),
      m_impl->jitterLate.load(std::memory_order_relaxed),
      m_impl->timingSamples.load(std::memory_order_relaxed),
      m_impl->decodeTotal.load(std::memory_order_relaxed),
      m_impl->decodeMax.load(std::memory_order_relaxed),
      m_impl->residenceTotal.load(std::memory_order_relaxed),
      m_impl->residenceMax.load(std::memory_order_relaxed),
      m_impl->needsReanchor.load(std::memory_order_relaxed),
      m_impl->reanchorDiscarded.load(std::memory_order_relaxed),
      m_impl->networkCorrection.load(std::memory_order_relaxed),
      playback.residenceFrames,
      playback.residenceNsTotal,
      playback.residenceNsMax,
      m_impl->feedbackUpdates.load(std::memory_order_relaxed),
      m_impl->feedbackConflicts.load(std::memory_order_relaxed),
      m_impl->feedbackInvalid.load(std::memory_order_relaxed),
      m_impl->feedbackCooldown.load(std::memory_order_relaxed)
  };
}
} // namespace deskflow::audio::engine
