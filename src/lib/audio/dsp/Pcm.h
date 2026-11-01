/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace deskflow::audio::dsp {

// The realtime SPSC paths require genuinely lock-free atomics, not a library fallback
// lock. Fail compilation on an unsupported target rather than silently blocking callbacks.
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// All PCM APIs use native float, interleaved, with frame counts (not sample counts).
struct PcmFormat
{
  std::uint32_t sampleRate = 48000;
  std::uint32_t channels = 2;

  [[nodiscard]] bool valid() const noexcept
  {
    return sampleRate > 0 && channels > 0 && channels <= 32;
  }
  [[nodiscard]] std::size_t samplesForFrames(std::size_t frames) const
  {
    if (!valid() || frames > std::numeric_limits<std::size_t>::max() / channels) {
      throw std::invalid_argument("Invalid PCM format or frame count");
    }
    return frames * channels;
  }
  [[nodiscard]] std::size_t framesForDuration(std::uint32_t milliseconds) const
  {
    const auto product = std::uint64_t(sampleRate) * milliseconds;
    if (!valid() || product % 1000 != 0 || product / 1000 > std::numeric_limits<std::size_t>::max()) {
      throw std::invalid_argument("Duration does not represent a whole PCM frame count");
    }
    return static_cast<std::size_t>(product / 1000);
  }
  [[nodiscard]] bool wholeFrames(std::span<const float> samples) const noexcept
  {
    return valid() && samples.size() % channels == 0;
  }
  bool operator==(const PcmFormat &) const = default;
};

inline float finiteSample(float value) noexcept
{
  return std::isfinite(value) ? value : 0.0f;
}
inline float sanitizedVolume(float value) noexcept
{
  return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

namespace detail {
inline std::size_t slotsForCapacity(std::size_t capacity)
{
  if (capacity == 0 || capacity == std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("PCM capacity must be positive and bounded");
  }
  return capacity + 1;
}
inline std::size_t checkedProduct(std::size_t a, std::size_t b)
{
  if (b == 0 || a > std::numeric_limits<std::size_t>::max() / b) {
    throw std::invalid_argument("PCM storage size overflow");
  }
  return a * b;
}
inline std::size_t distance(std::size_t read, std::size_t write, std::size_t slots) noexcept
{
  return write >= read ? write - read : slots - read + write;
}
} // namespace detail

struct PcmBlockInfo
{
  std::uint64_t sequence = 0;
  std::uint64_t timestampFrames = 0;
  std::size_t frames = 0;
  bool discontinuity = false;
  // Stream configuration generation; old-format blocks must never be decoded as a new generation.
  std::uint64_t generation = 0;
  // Optional caller monotonic timestamp of the enqueue/completing capture callback.
  // Zero means unavailable; separate from media timestampFrames and its clock domain.
  std::uint64_t enqueuedNs = 0;
  // First contributing callback, allowing framing time to be measured separately.
  std::uint64_t capturedNs = 0;
};

// Exactly one producer and one consumer. Construction/destruction require both stopped.
// No allocations after construction; push copies caller data and pop copies into caller storage.
// Full -> reject newest. NEVER advances the consumer index from the producer.
// Consumer can discard expired blocks using peek()/discard(); sequence gaps expose losses.
class SpscPcmQueue
{
public:
  SpscPcmQueue(PcmFormat format, std::size_t capacityBlocks, std::size_t maxFramesPerBlock)
      : m_format(format),
        m_slots(detail::slotsForCapacity(capacityBlocks)),
        m_stride(format.samplesForFrames(maxFramesPerBlock)),
        m_samples(detail::checkedProduct(m_slots, m_stride)),
        m_info(m_slots)
  {
  }

  [[nodiscard]] bool push(std::span<const float> samples, PcmBlockInfo info) noexcept
  {
    if (samples.empty() || !m_format.wholeFrames(samples) || samples.size() > m_stride) {
      return false;
    }
    const auto write = m_write.load(std::memory_order_relaxed);
    const auto next = (write + 1) % m_slots;
    if (next == m_read.load(std::memory_order_acquire)) {
      return false;
    }
    std::copy(samples.begin(), samples.end(), m_samples.begin() + write * m_stride);
    info.frames = samples.size() / m_format.channels;
    m_info[write] = info;
    m_write.store(next, std::memory_order_release);
    return true;
  }

  // Consumer-only. No borrowed spans escape the queue.
  [[nodiscard]] bool peek(PcmBlockInfo &info) const noexcept
  {
    const auto read = m_read.load(std::memory_order_relaxed);
    if (read == m_write.load(std::memory_order_acquire)) {
      return false;
    }
    info = m_info[read];
    return true;
  }
  [[nodiscard]] bool pop(std::span<float> destination, PcmBlockInfo &info) noexcept
  {
    if (!peek(info) || destination.size() < info.frames * m_format.channels) {
      return false;
    }
    const auto read = m_read.load(std::memory_order_relaxed);
    std::copy_n(m_samples.begin() + read * m_stride, info.frames * m_format.channels, destination.begin());
    m_read.store((read + 1) % m_slots, std::memory_order_release);
    return true;
  }
  [[nodiscard]] bool discard() noexcept
  {
    PcmBlockInfo ignored;
    if (!peek(ignored)) {
      return false;
    }
    const auto read = m_read.load(std::memory_order_relaxed);
    m_read.store((read + 1) % m_slots, std::memory_order_release);
    return true;
  }
  [[nodiscard]] std::size_t capacityBlocks() const noexcept
  {
    return m_slots - 1;
  }
  // Consumer-side instantaneous observation, not a cross-thread telemetry snapshot.
  [[nodiscard]] std::size_t queuedBlocks() const noexcept
  {
    return detail::distance(m_read.load(std::memory_order_relaxed), m_write.load(std::memory_order_acquire), m_slots);
  }

private:
  PcmFormat m_format;
  const std::size_t m_slots;
  const std::size_t m_stride;
  std::vector<float> m_samples;
  std::vector<PcmBlockInfo> m_info;
  alignas(64) std::atomic<std::size_t> m_read{0};
  alignas(64) std::atomic<std::size_t> m_write{0};
};

// Decoded/device-format PCM only: NOT a network jitter buffer. One writer, one reader.
// All-or-nothing writes reject newest on overflow. Consumer owns latency trimming.
class SpscPcmRing
{
public:
  SpscPcmRing(PcmFormat format, std::size_t capacityFrames)
      : m_format(format),
        m_slots(detail::slotsForCapacity(capacityFrames)),
        m_samples(format.samplesForFrames(m_slots)),
        m_discontinuities(m_slots),
        m_writtenNs(m_slots)
  {
  }
  [[nodiscard]] bool
  write(std::span<const float> samples, bool discontinuity = false, std::uint64_t writtenNs = 0) noexcept
  {
    if (samples.empty() || !m_format.wholeFrames(samples)) {
      return false;
    }
    const auto frames = samples.size() / m_format.channels;
    auto write = m_write.load(std::memory_order_relaxed);
    const auto used = detail::distance(m_read.load(std::memory_order_acquire), write, m_slots);
    if (frames > capacityFrames() - used) {
      return false;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
      std::copy_n(
          samples.begin() + frame * m_format.channels, m_format.channels, m_samples.begin() + write * m_format.channels
      );
      m_discontinuities[write] = discontinuity && frame == 0;
      m_writtenNs[write] = writtenNs;
      write = (write + 1) % m_slots;
    }
    m_write.store(write, std::memory_order_release);
    return true;
  }
  // Consumer-only; exactly one frame, with discontinuity marker on that frame.
  [[nodiscard]] bool readFrame(std::span<float> frame, bool &discontinuity, std::uint64_t *writtenNs = nullptr) noexcept
  {
    if (frame.size() != m_format.channels) {
      return false;
    }
    const auto read = m_read.load(std::memory_order_relaxed);
    if (read == m_write.load(std::memory_order_acquire)) {
      return false;
    }
    std::copy_n(m_samples.begin() + read * m_format.channels, m_format.channels, frame.begin());
    discontinuity = m_discontinuities[read] != 0;
    if (writtenNs)
      *writtenNs = m_writtenNs[read];
    m_read.store((read + 1) % m_slots, std::memory_order_release);
    return true;
  }
  [[nodiscard]] std::size_t discardTo(std::size_t retainedFrames) noexcept
  {
    const auto read = m_read.load(std::memory_order_relaxed);
    const auto used = queuedFrames();
    const auto dropped = used > retainedFrames ? used - retainedFrames : 0;
    m_read.store((read + dropped) % m_slots, std::memory_order_release);
    return dropped;
  }
  [[nodiscard]] std::size_t queuedFrames() const noexcept
  {
    return detail::distance(m_read.load(std::memory_order_relaxed), m_write.load(std::memory_order_acquire), m_slots);
  }
  // Producer-side snapshot: own write is relaxed, reader progress acquired. The consumer
  // may free additional frames immediately afterwards, so this is a conservative watermark.
  [[nodiscard]] std::size_t queuedFramesForWriter() const noexcept
  {
    const auto write = m_write.load(std::memory_order_relaxed);
    return detail::distance(m_read.load(std::memory_order_acquire), write, m_slots);
  }
  [[nodiscard]] std::size_t capacityFrames() const noexcept
  {
    return m_slots - 1;
  }
  [[nodiscard]] PcmFormat format() const noexcept
  {
    return m_format;
  }

private:
  PcmFormat m_format;
  const std::size_t m_slots;
  std::vector<float> m_samples;
  std::vector<std::uint8_t> m_discontinuities;
  std::vector<std::uint64_t> m_writtenNs;
  alignas(64) std::atomic<std::size_t> m_read{0};
  alignas(64) std::atomic<std::size_t> m_write{0};
};
} // namespace deskflow::audio::dsp
