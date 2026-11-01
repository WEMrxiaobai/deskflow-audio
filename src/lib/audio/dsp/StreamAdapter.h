/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Pcm.h"
#include <array>

namespace deskflow::audio::dsp {

struct AdapterProgress
{
  std::size_t inputFrames = 0;
  std::size_t outputFrames = 0;
  bool valid = true;
};

// Allocation-free streaming linear interpolation baseline, NOT a band-limited/music-quality
// resampler. Downsampling requires externally band-limited input to avoid aliasing. Explicit
// support: interleaved f32, 8..192 kHz, mono/stereo. Mono->stereo duplicates; stereo->mono
// averages L/R. Up to one input-frame lookahead; no flush padding or repeated final frame.
// Retained interpolation state crosses callbacks. Phase is an integer rational accumulator,
// not a growing floating-point absolute offset. Changing correction never resets phase.
// Single-thread-owned. Construction/configuration happen outside the realtime callback.
class StreamAdapter
{
public:
  StreamAdapter(PcmFormat input, PcmFormat output) : m_input(input), m_output(output)
  {
    if (!supported(input) || !supported(output)) {
      throw std::invalid_argument("StreamAdapter supports 8..192kHz mono/stereo f32 only");
    }
    m_denominator = std::uint64_t(output.sampleRate) * 1000000000ULL;
    (void)setRateCorrectionPpm(0);
  }

  [[nodiscard]] static bool supported(PcmFormat format) noexcept
  {
    return format.sampleRate >= 8000 && format.sampleRate <= 192000 && (format.channels == 1 || format.channels == 2);
  }
  // Positive ppm consumes input faster (e.g. a high receive-buffer water level).
  // Range +/-5000 ppm, quantized to .001 ppm; returns false and preserves state for invalid input.
  [[nodiscard]] bool setRateCorrectionPpm(double ppm) noexcept
  {
    if (!std::isfinite(ppm) || std::abs(ppm) > 5000.0) {
      return false;
    }
    const auto partsPerBillion = static_cast<std::int64_t>(std::llround(ppm * 1000.0));
    m_step = std::uint64_t(m_input.sampleRate) * static_cast<std::uint64_t>(1000000000LL + partsPerBillion);
    return true;
  }
  void reset() noexcept
  {
    m_phase = 0;
    m_haveLeft = m_haveRight = false;
  }
  [[nodiscard]] PcmFormat inputFormat() const noexcept
  {
    return m_input;
  }
  [[nodiscard]] PcmFormat outputFormat() const noexcept
  {
    return m_output;
  }

  // Both spans must contain whole frames. Never drops unread input or overwrites output.
  // Advance input by inputFrames and output by outputFrames, and retry when output was full.
  // Empty input may still emit an already-buffered interpolation frame; an empty output
  // consumes nothing. Invalid calls do not mutate state.
  [[nodiscard]] AdapterProgress process(std::span<const float> input, std::span<float> output) noexcept
  {
    AdapterProgress result;
    if (!m_input.wholeFrames(input) || output.size() % m_output.channels != 0) {
      result.valid = false;
      return result;
    }
    const auto inputFrames = input.size() / m_input.channels;
    const auto outputFrames = output.size() / m_output.channels;
    auto readMapped = [&](std::array<float, 2> &frame) {
      if (result.inputFrames == inputFrames) {
        return false;
      }
      const auto offset = result.inputFrames++ * m_input.channels;
      const auto left = finiteSample(input[offset]);
      const auto right = m_input.channels == 2 ? finiteSample(input[offset + 1]) : left;
      if (m_output.channels == 1) {
        frame[0] = static_cast<float>((double(left) + double(right)) * 0.5);
      } else {
        frame[0] = left;
        frame[1] = right;
      }
      return true;
    };
    while (result.outputFrames < outputFrames) {
      if (!m_haveLeft) {
        if (!readMapped(m_left)) {
          break;
        }
        m_haveLeft = true;
      }
      while (m_phase >= m_denominator) {
        if (m_haveRight) {
          m_left = m_right;
          m_haveRight = false;
        } else if (!readMapped(m_left)) {
          return result;
        }
        m_phase -= m_denominator;
      }
      if (m_phase != 0 && !m_haveRight) {
        if (!readMapped(m_right)) {
          break;
        }
        m_haveRight = true;
      }
      const double fraction = double(m_phase) / double(m_denominator);
      for (std::size_t channel = 0; channel < m_output.channels; ++channel) {
        const auto value = m_phase == 0
                               ? double(m_left[channel])
                               : double(m_left[channel]) * (1.0 - fraction) + double(m_right[channel]) * fraction;
        output[result.outputFrames * m_output.channels + channel] = static_cast<float>(value);
      }
      ++result.outputFrames;
      m_phase += m_step;
    }
    return result;
  }

private:
  PcmFormat m_input, m_output;
  std::uint64_t m_denominator = 1, m_step = 1, m_phase = 0;
  std::array<float, 2> m_left{}, m_right{};
  bool m_haveLeft = false, m_haveRight = false;
};
} // namespace deskflow::audio::dsp
