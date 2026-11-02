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

// Windows WAVEFORMATEXTENSIBLE order, independent of Windows headers. Only
// layouts with an unambiguous mapping are accepted. Downmix includes center,
// LFE, back and side channels with fixed headroom for fully correlated signals.
inline bool supportedChannelLayout(PcmFormat f) noexcept
{
  if (f.channels == 1)
    return f.channelMask == 0 || f.channelMask == 0x4;
  if (f.channels == 2)
    return f.channelMask == 0 || f.channelMask == 0x3;
  return (f.channels == 6 && (f.channelMask == 0x3f || f.channelMask == 0x60f)) ||
         (f.channels == 8 && f.channelMask == 0x63f);
}
inline std::array<float, 2> stereoFrame(PcmFormat f, const float *samples) noexcept
{
  const auto left = finiteSample(samples[0]);
  if (f.channels == 1)
    return {left, left};
  if (f.channels == 2)
    return {left, finiteSample(samples[1])};
  constexpr double surround = 0.7071067811865476;
  const double scale = 1.0 / (1.0 + surround + 0.5 + surround * (f.channels == 8 ? 2.0 : 1.0));
  double l = 0, r = 0;
  unsigned index = 0;
  for (unsigned bit = 0; bit < 32; ++bit) {
    if (!(f.channelMask & (std::uint32_t(1) << bit)))
      continue;
    const auto sample = double(finiteSample(samples[index++]));
    switch (bit) {
    case 0: l += sample; break; // front left
    case 1: r += sample; break; // front right
    case 2: l += sample * surround; r += sample * surround; break; // center
    case 3: l += sample * 0.5; r += sample * 0.5; break; // LFE
    case 4: case 9: l += sample * surround; break; // back/side left
    case 5: case 10: r += sample * surround; break; // back/side right
    }
  }
  return {float(l * scale), float(r * scale)};
}
inline void writeStereoFrame(PcmFormat f, const std::array<float, 2> &stereo, float *output) noexcept
{
  if (f.channels == 1) {
    output[0] = float((double(stereo[0]) + double(stereo[1])) * 0.5);
    return;
  }
  output[0] = stereo[0];
  output[1] = stereo[1];
  // Stereo is rendered on the physical front L/R speakers; other speakers
  // are silent, avoiding invented center, surround or unfiltered LFE content.
  std::fill(output + 2, output + f.channels, 0.0f);
}

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
      throw std::invalid_argument("Unsupported audio format: use 8..192kHz mono/stereo or a standard 5.1/7.1 speaker layout with channel mask");
    }
    m_denominator = std::uint64_t(output.sampleRate) * 1000000000ULL;
    (void)setRateCorrectionPpm(0);
  }

  [[nodiscard]] static bool supported(PcmFormat format) noexcept
  {
    return format.sampleRate >= 8000 && format.sampleRate <= 192000 && supportedChannelLayout(format);
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
      frame = stereoFrame(m_input, input.data() + offset);
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
      std::array<float, 2> frame{};
      for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto value = m_phase == 0
                               ? double(m_left[channel])
                               : double(m_left[channel]) * (1.0 - fraction) + double(m_right[channel]) * fraction;
        frame[channel] = static_cast<float>(value);
      }
      writeStereoFrame(m_output, frame, output.data() + result.outputFrames * m_output.channels);
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
