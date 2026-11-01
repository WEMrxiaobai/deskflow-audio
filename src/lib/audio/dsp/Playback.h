/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Pcm.h"
#include <array>

namespace deskflow::audio::dsp {

struct PlaybackStats
{
  std::uint64_t rejectedFrames = 0;
  std::uint64_t trimmedFrames = 0;
  std::uint64_t underrunFrames = 0;
  std::uint64_t underrunEvents = 0;
  std::uint64_t residenceFrames = 0, residenceNsTotal = 0, residenceNsMax = 0;
};

// Device-format PCM, not packet jitter buffering. One writer and one callback reader.
// No callback allocations or locks. Volume/mute can be set from a control thread.
// Rejected writes mark the next accepted block as a discontinuity. The reader owns
// trimming. Stream reset/format reconstruction requires the writer to be quiescent.
class PlaybackSource
{
public:
  PlaybackSource(PcmFormat format, std::size_t capacityFrames, std::size_t smoothFrames, float initialVolume = 0.5f)
      : m_ring(format, capacityFrames),
        m_format(format),
        m_smoothFrames(std::max(std::size_t(1), smoothFrames)),
        m_volume(sanitizedVolume(initialVolume))
  {
  }
  [[nodiscard]] bool write(std::span<const float> pcm, bool discontinuity = false, std::uint64_t writtenNs = 0) noexcept
  {
    if (!m_format.wholeFrames(pcm)) {
      return false;
    }
    if (!m_ring.write(pcm, discontinuity || m_pendingBreak, writtenNs)) {
      m_rejected.fetch_add(pcm.size() / m_format.channels, std::memory_order_relaxed);
      m_pendingBreak = true;
      return false;
    }
    m_pendingBreak = false;
    return true;
  }
  void setVolume(float volume) noexcept
  {
    m_volume.store(sanitizedVolume(volume), std::memory_order_relaxed);
  }
  void setMuted(bool muted) noexcept
  {
    m_muted.store(muted, std::memory_order_relaxed);
  }
  [[nodiscard]] bool muted() const noexcept
  {
    return m_muted.load(std::memory_order_relaxed);
  }
  [[nodiscard]] PcmFormat format() const noexcept
  {
    return m_format;
  }
  [[nodiscard]] std::size_t queuedFrames() const noexcept
  {
    return m_ring.queuedFrames();
  }
  [[nodiscard]] std::size_t capacityFrames() const noexcept
  {
    return m_ring.capacityFrames();
  }
  // Producer-only acquire observation, useful for feedback in the decode worker.
  [[nodiscard]] std::size_t queuedFramesForWriter() const noexcept
  {
    return m_ring.queuedFramesForWriter();
  }
  [[nodiscard]] std::size_t writableFrames() const noexcept
  {
    return m_ring.capacityFrames() - queuedFramesForWriter();
  }
  // Reader-only. No producer index is modified. Smooth the retained head after a trim.
  [[nodiscard]] std::size_t trimTo(std::size_t retainedFrames) noexcept
  {
    const auto dropped = m_ring.discardTo(retainedFrames);
    if (dropped != 0) {
      m_readerBreak = true;
      m_trimmed.fetch_add(dropped, std::memory_order_relaxed);
    }
    return dropped;
  }
  // Reader-only with writer stopped, e.g. before accepting a new stream generation.
  // Does not change volume or mute: reconnect must not unmute a user-muted source.
  void resetStream() noexcept
  {
    (void)trimTo(0);
    m_pendingBreak = true;
    m_readerBreak = true;
    m_hadData = false;
    m_transitionRemaining = 0;
    m_last.fill(0);
    m_anchor.fill(0);
    m_gain = 0;
  }

  // Reader-only. Returns whether actual PCM was read; false still produces a controlled
  // fade to zero. Exactly N frames reach the target, including across callback boundaries.
  [[nodiscard]] bool renderFrame(std::span<float> output, std::uint64_t nowNs = 0) noexcept
  {
    if (output.size() != m_format.channels) {
      return false;
    }
    std::array<float, 32> input{};
    bool discontinuity = false;
    std::uint64_t writtenNs = 0;
    const bool haveData =
        m_ring.readFrame(std::span<float>(input.data(), m_format.channels), discontinuity, &writtenNs);
    if (haveData && writtenNs != 0 && nowNs >= writtenNs) {
      const auto residence = nowNs - writtenNs;
      m_residenceFrames.fetch_add(1, std::memory_order_relaxed);
      m_residenceTotal.fetch_add(residence, std::memory_order_relaxed);
      if (residence > m_residenceMax.load(std::memory_order_relaxed))
        m_residenceMax.store(residence, std::memory_order_relaxed);
    }
    if ((haveData && (!m_hadData || discontinuity || m_readerBreak)) || (!haveData && m_hadData)) {
      m_anchor = m_last;
      m_transitionRemaining = m_smoothFrames;
    }
    if (!haveData) {
      m_underrunFrames.fetch_add(1, std::memory_order_relaxed);
      if (m_hadData) {
        m_underrunEvents.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (haveData) {
      m_readerBreak = false;
    }
    const auto target = muted() ? 0.0f : m_volume.load(std::memory_order_relaxed);
    const auto gainStep = 1.0 / double(m_smoothFrames);
    m_gain += std::clamp(double(target) - m_gain, -gainStep, gainStep);
    const auto fraction =
        m_transitionRemaining == 0 ? 1.0 : double(m_smoothFrames - m_transitionRemaining + 1) / double(m_smoothFrames);
    for (std::size_t channel = 0; channel < m_format.channels; ++channel) {
      const auto sample = haveData ? double(finiteSample(input[channel])) : 0.0;
      m_last[channel] = static_cast<float>(double(m_anchor[channel]) * (1.0 - fraction) + sample * fraction);
      output[channel] = static_cast<float>(double(m_last[channel]) * m_gain);
    }
    if (m_transitionRemaining != 0) {
      --m_transitionRemaining;
    }
    m_hadData = haveData;
    return haveData;
  }
  [[nodiscard]] bool render(std::span<float> output, std::uint64_t nowNs = 0) noexcept
  {
    if (output.size() % m_format.channels != 0) {
      return false;
    }
    for (std::size_t offset = 0; offset < output.size(); offset += m_format.channels) {
      (void)renderFrame(output.subspan(offset, m_format.channels), nowNs);
    }
    return true;
  }
  [[nodiscard]] PlaybackStats stats() const noexcept
  {
    return {m_rejected.load(std::memory_order_relaxed),        m_trimmed.load(std::memory_order_relaxed),
            m_underrunFrames.load(std::memory_order_relaxed),  m_underrunEvents.load(std::memory_order_relaxed),
            m_residenceFrames.load(std::memory_order_relaxed), m_residenceTotal.load(std::memory_order_relaxed),
            m_residenceMax.load(std::memory_order_relaxed)};
  }

private:
  SpscPcmRing m_ring;
  PcmFormat m_format;
  const std::size_t m_smoothFrames;
  std::atomic<float> m_volume;
  std::atomic<bool> m_muted{false};
  std::atomic<std::uint64_t> m_rejected{0}, m_trimmed{0}, m_underrunFrames{0}, m_underrunEvents{0};
  std::atomic<std::uint64_t> m_residenceFrames{0}, m_residenceTotal{0}, m_residenceMax{0};
  bool m_pendingBreak = false; // writer-owned
  bool m_readerBreak = false, m_hadData = false;
  std::size_t m_transitionRemaining = 0;
  double m_gain = 0;
  std::array<float, 32> m_last{}, m_anchor{};
};

// Bounded, independent-source sum followed by hard peak protection, not loudness
// normalization or ducking. No lookahead. This protects remote mix only, not other
// Windows application audio. Max 8 sources, max 32 channels, caller-provided output.
// Invalid configuration clears output and consumes no source samples.
[[nodiscard]] inline bool mixSources(
    std::span<PlaybackSource *const> sources, PcmFormat format, std::span<float> output, float ceiling = 0.98f,
    std::uint64_t nowNs = 0
) noexcept
{
  if (!format.valid() || output.size() % format.channels != 0 || sources.size() > 8 || !std::isfinite(ceiling) ||
      ceiling <= 0 || ceiling > 1) {
    std::fill(output.begin(), output.end(), 0.0f);
    return false;
  }
  for (auto *source : sources) {
    if (source == nullptr || source->format() != format) {
      std::fill(output.begin(), output.end(), 0.0f);
      return false;
    }
  }
  std::array<float, 32> frame{};
  std::array<double, 32> sum{};
  for (std::size_t offset = 0; offset < output.size(); offset += format.channels) {
    sum.fill(0);
    for (auto *source : sources) {
      (void)source->renderFrame(std::span<float>(frame.data(), format.channels), nowNs);
      for (std::size_t channel = 0; channel < format.channels; ++channel) {
        sum[channel] += finiteSample(frame[channel]);
      }
    }
    for (std::size_t channel = 0; channel < format.channels; ++channel) {
      output[offset + channel] = static_cast<float>(std::clamp(sum[channel], -double(ceiling), double(ceiling)));
    }
  }
  return true;
}
} // namespace deskflow::audio::dsp
