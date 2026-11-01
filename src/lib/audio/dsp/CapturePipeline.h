/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "BandLimitedAdapter.h"
#include "StreamAdapter.h"
#include <memory>

namespace deskflow::audio::dsp {

struct CaptureStats
{
  std::uint64_t producedBlocks = 0;
  std::uint64_t droppedBlocks = 0;
  std::uint64_t droppedPartialFrames = 0;
  std::uint64_t expiredBlocks = 0;
  std::uint64_t wrongGenerationBlocks = 0;
};

// One callback producer, one encoder consumer. Input may have any callback length.
// All storage is allocated in the constructor. Full queue rejects newest; the consumer
// (not producer) discards expired backlog. Sequence/timestamp advance even on rejection.
// Timestamp unit is output PCM frames, starting at the configured media timestamp.
class CapturePipeline
{
public:
  CapturePipeline(
      PcmFormat input, PcmFormat output, std::uint32_t milliseconds, std::size_t capacityBlocks,
      std::uint64_t generation = 0, std::uint64_t timestampFrames = 0, bool bandLimited = false
  )
      : m_adapter(input, output),
        m_bandLimited(bandLimited ? std::make_unique<BandLimitedAdapter>(input, output) : nullptr),
        m_format(output),
        m_frameCount(frameCount(output, milliseconds)),
        m_queue(output, capacityBlocks, m_frameCount),
        m_partial(output.samplesForFrames(m_frameCount)),
        m_scratch(output.samplesForFrames(512)),
        m_generation(generation),
        m_timestamp(timestampFrames),
        m_latestTimestamp(timestampFrames)
  {
  }

  // Producer only. Existing queued blocks deliberately keep their old generation, and
  // consumer popForGeneration() filters them. Quiesce both sides before destroying/reconfiguring.
  void resetStream(std::uint64_t generation, std::uint64_t timestampFrames = 0) noexcept
  {
    dropPartial();
    m_adapter.reset();
    if (m_bandLimited)
      m_bandLimited->reset();
    m_generation = generation;
    m_timestamp = timestampFrames;
    m_latestTimestamp.store(timestampFrames, std::memory_order_release);
    m_sequence = 0;
    m_discontinuity = true;
  }
  [[nodiscard]] bool setRateCorrectionPpm(double ppm) noexcept
  {
    return m_bandLimited ? m_bandLimited->setRateCorrectionPpm(ppm) : m_adapter.setRateCorrectionPpm(ppm);
  }
  [[nodiscard]] std::size_t latencyInputFrames() const noexcept
  {
    return m_bandLimited ? m_bandLimited->latencyInputFrames() : 0;
  }
  // nowNs is the caller's monotonic callback clock; zero means unavailable.
  [[nodiscard]] bool push(std::span<const float> input, bool discontinuity = false, std::uint64_t nowNs = 0) noexcept
  {
    if (!m_adapter.inputFormat().wholeFrames(input)) {
      return false;
    }
    if (discontinuity) {
      dropPartial();
      m_adapter.reset();
      if (m_bandLimited)
        m_bandLimited->reset();
      m_discontinuity = true;
    }
    // Drain buffered interpolation too. Each successful iteration consumes or produces;
    // work is bounded by callback size and supported rate ratio, never by queue availability.
    for (;;) {
      const auto progress =
          m_bandLimited ? m_bandLimited->process(input, m_scratch) : m_adapter.process(input, m_scratch);
      input = input.subspan(progress.inputFrames * m_adapter.inputFormat().channels);
      append(std::span<const float>(m_scratch.data(), progress.outputFrames * m_format.channels), nowNs);
      if (progress.inputFrames == 0 && progress.outputFrames == 0) {
        break;
      }
    }
    m_latestTimestamp.store(m_timestamp, std::memory_order_release);
    return true;
  }
  [[nodiscard]] std::uint64_t latestTimestampFrames() const noexcept
  {
    return m_latestTimestamp.load(std::memory_order_acquire);
  }

  // Raw queue access for workers with their own media-age policy. Consumer only.
  [[nodiscard]] SpscPcmQueue &queue() noexcept
  {
    return m_queue;
  }
  // Consumer-only: age is measured from block start; equality at deadline is expired.
  [[nodiscard]] bool popForGeneration(
      std::span<float> output, PcmBlockInfo &info, std::uint64_t generation, std::uint64_t nowFrames,
      std::uint64_t maxAgeFrames
  ) noexcept
  {
    while (m_queue.peek(info)) {
      if (info.generation != generation) {
        (void)m_queue.discard();
        m_wrongGeneration.fetch_add(1, std::memory_order_relaxed);
      } else if (nowFrames >= info.timestampFrames && nowFrames - info.timestampFrames >= maxAgeFrames) {
        (void)m_queue.discard();
        m_expired.fetch_add(1, std::memory_order_relaxed);
      } else {
        return m_queue.pop(output, info);
      }
    }
    return false;
  }
  [[nodiscard]] PcmFormat format() const noexcept
  {
    return m_format;
  }
  [[nodiscard]] std::size_t framesPerBlock() const noexcept
  {
    return m_frameCount;
  }
  [[nodiscard]] CaptureStats stats() const noexcept
  {
    return {
        m_produced.load(std::memory_order_relaxed), m_dropped.load(std::memory_order_relaxed),
        m_partialDropped.load(std::memory_order_relaxed), m_expired.load(std::memory_order_relaxed),
        m_wrongGeneration.load(std::memory_order_relaxed)
    };
  }

private:
  static std::size_t frameCount(PcmFormat format, std::uint32_t milliseconds)
  {
    if (milliseconds != 5 && milliseconds != 10) {
      throw std::invalid_argument("Capture framing supports exactly 5 or 10ms");
    }
    return format.framesForDuration(milliseconds);
  }
  void dropPartial() noexcept
  {
    m_partialDropped.fetch_add(m_filled, std::memory_order_relaxed);
    m_filled = 0;
  }
  void append(std::span<const float> input, std::uint64_t nowNs) noexcept
  {
    auto frames = input.size() / m_format.channels;
    while (frames != 0) {
      if (m_filled == 0) {
        m_blockTimestamp = m_timestamp;
        m_blockEnqueuedNs = nowNs;
      }
      const auto count = std::min(frames, m_frameCount - m_filled);
      std::copy_n(input.begin(), count * m_format.channels, m_partial.begin() + m_filled * m_format.channels);
      input = input.subspan(count * m_format.channels);
      m_filled += count;
      m_timestamp += count;
      frames -= count;
      if (m_filled == m_frameCount) {
        PcmBlockInfo info{m_sequence++, m_blockTimestamp, m_frameCount, m_discontinuity, m_generation,
                          nowNs,        m_blockEnqueuedNs};
        if (m_queue.push(m_partial, info)) {
          m_produced.fetch_add(1, std::memory_order_relaxed);
          m_discontinuity = false;
        } else {
          m_dropped.fetch_add(1, std::memory_order_relaxed);
          m_discontinuity = true;
        }
        m_filled = 0;
      }
    }
  }

  StreamAdapter m_adapter;
  std::unique_ptr<BandLimitedAdapter> m_bandLimited; // Constructed off callback; no runtime switching.
  PcmFormat m_format;
  std::size_t m_frameCount;
  SpscPcmQueue m_queue;
  std::vector<float> m_partial, m_scratch;
  std::size_t m_filled = 0;
  std::uint64_t m_generation, m_timestamp, m_sequence = 0, m_blockTimestamp = 0, m_blockEnqueuedNs = 0;
  std::atomic<std::uint64_t> m_latestTimestamp;
  bool m_discontinuity = true;
  std::atomic<std::uint64_t> m_produced{0}, m_dropped{0}, m_partialDropped{0}, m_expired{0}, m_wrongGeneration{0};
};
} // namespace deskflow::audio::dsp
