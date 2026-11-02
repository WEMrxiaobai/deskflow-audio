/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "StreamAdapter.h" // Shared PcmFormat and AdapterProgress; does not use the linear resampler.
#include <numbers>

namespace deskflow::audio::dsp {

// Streaming, interleaved-f32 windowed-sinc resampler. Single-thread-owned, like StreamAdapter.
// Construction allocates and calculates all FIR coefficients; process/reset/setRateCorrectionPpm
// allocate nothing. Both anti-alias filtering and interpolation/image rejection are performed
// by the FIR (there is no linear-sample-interpolation stage).
//
// 64 taps at ratios <= 1, ceil(64 * inputRate/outputRate) taps (rounded up to even) otherwise:
// 96->48 uses 128 taps, 192->48 uses 256, worst case 192->8 uses 1536. The Blackman-windowed
// sinc cutoff is 0.91 * min(inputNyquist, outputNyquist / 1.005), reserving the +5000ppm case.
// This deliberately trades the top ~10% of the smaller Nyquist band for stopband rejection.
// 1025 phase rows, linearly interpolated, include the phase=1 endpoint. Storage is bounded by
// ~6.1 MiB coefficients + 12 KiB history at the largest ratio. At normal audio rates the
// fractional-delay table error is much smaller than the Blackman stopband (~74 dB).
//
// Output frame k represents source position t starting at zero, not a repeated/padded tail.
// Negative-time history is zero. To evaluate t the adapter waits through floor(t)+taps/2;
// latencyInputFrames() is this fixed lookahead/group delay in input frames. Thus the first
// output needs latencyInputFrames()+1 real input frames. There is NO implicit end padding:
// an empty input may drain already-buffered output, but cannot synthesize a missing lookahead.
// reset discards history/phase, keeps the selected ppm and never reallocates.
class BandLimitedAdapter
{
public:
  BandLimitedAdapter(PcmFormat input, PcmFormat output) : m_input(input), m_output(output)
  {
    if (!supported(input) || !supported(output)) {
      throw std::invalid_argument("Unsupported audio format: use 8..192kHz mono/stereo or a standard 5.1/7.1 speaker layout with channel mask");
    }
    const double ratio = double(input.sampleRate) / output.sampleRate;
    m_taps = static_cast<std::size_t>(std::ceil(64.0 * std::max(1.0, ratio)));
    m_taps += m_taps % 2;
    m_coefficients.resize((phaseCount + 1) * m_taps);
    m_history.resize(m_taps);
    const double cutoff = 0.91 / std::max(1.0, ratio * 1.005);
    const double half = double(m_taps / 2);
    for (std::size_t phase = 0; phase <= phaseCount; ++phase) {
      const double fraction = double(phase) / double(phaseCount);
      double sum = 0;
      for (std::size_t tap = 0; tap < m_taps; ++tap) {
        const double x = double(tap) - half + 1.0 - fraction;
        const double angle = std::numbers::pi * cutoff * x;
        const double sinc = std::abs(angle) < 1e-15 ? 1.0 : std::sin(angle) / angle;
        const double window = std::abs(x) >= half ? 0.0
                                                  : 0.42 + 0.5 * std::cos(std::numbers::pi * x / half) +
                                                        0.08 * std::cos(2.0 * std::numbers::pi * x / half);
        const double value = cutoff * sinc * window;
        m_coefficients[phase * m_taps + tap] = static_cast<float>(value);
        sum += value;
      }
      // Independently normalize every phase to unity DC, including the endpoint row.
      for (std::size_t tap = 0; tap < m_taps; ++tap) {
        auto &value = m_coefficients[phase * m_taps + tap];
        value = static_cast<float>(double(value) / sum);
      }
    }
    m_denominator = std::uint64_t(output.sampleRate) * 1000000000ULL;
    (void)setRateCorrectionPpm(0);
    reset();
  }

  [[nodiscard]] static bool supported(PcmFormat format) noexcept
  {
    return StreamAdapter::supported(format);
  }
  [[nodiscard]] PcmFormat inputFormat() const noexcept
  {
    return m_input;
  }
  [[nodiscard]] PcmFormat outputFormat() const noexcept
  {
    return m_output;
  }
  [[nodiscard]] std::size_t latencyInputFrames() const noexcept
  {
    return m_taps / 2;
  }

  // Positive ppm consumes input faster; .001 ppm quantization, +/-5000 ppm inclusive.
  // Only the rational step changes: coefficients, phase and buffered audio stay unchanged.
  [[nodiscard]] bool setRateCorrectionPpm(double ppm) noexcept
  {
    if (!std::isfinite(ppm) || std::abs(ppm) > 5000.0) {
      return false;
    }
    const auto ppb = static_cast<std::int64_t>(std::llround(ppm * 1000.0));
    m_step = std::uint64_t(m_input.sampleRate) * static_cast<std::uint64_t>(1000000000LL + ppb);
    return true;
  }

  void reset() noexcept
  {
    std::fill(m_history.begin(), m_history.end(), std::array<float, 2>{});
    m_write = 0;
    m_phase = 0;
    m_pending = latencyInputFrames() + 1;
  }

  // Whole interleaved frames only. Invalid spans do not change state. Empty output consumes
  // nothing. Input is consumed only as needed for requested output (initial lookahead+1,
  // thereafter at most ceil(inputRate/outputRate * 1.005) frames per output). The caller must
  // retry all unread input, even if the destination filled; no unbounded internal input queue.
  [[nodiscard]] AdapterProgress process(std::span<const float> input, std::span<float> output) noexcept
  {
    AdapterProgress result;
    if (!m_input.wholeFrames(input) || output.size() % m_output.channels != 0) {
      result.valid = false;
      return result;
    }
    const auto inputFrames = input.size() / m_input.channels;
    const auto outputFrames = output.size() / m_output.channels;
    while (result.outputFrames < outputFrames) {
      while (m_pending != 0) {
        if (result.inputFrames == inputFrames) {
          return result;
        }
        const auto offset = result.inputFrames++ * m_input.channels;
        m_history[m_write] = stereoFrame(m_input, input.data() + offset);
        if (++m_write == m_taps) {
          m_write = 0;
        }
        --m_pending;
      }
      // m_phase is always < denominator: no growing absolute floating-point position and
      // no callback-dependent rounding. Multiplication by 1024 fits uint64 at 192 kHz.
      const auto scaled = m_phase * phaseCount;
      const auto row = static_cast<std::size_t>(scaled / m_denominator);
      const double fraction = double(scaled % m_denominator) / double(m_denominator);
      const float *first = m_coefficients.data() + row * m_taps;
      const float *second = first + m_taps;
      std::array<double, 2> sum{};
      auto index = m_write; // oldest retained input frame
      for (std::size_t tap = 0; tap < m_taps; ++tap) {
        const double coefficient = double(first[tap]) + fraction * (double(second[tap]) - double(first[tap]));
        for (std::size_t channel = 0; channel < 2; ++channel) {
          sum[channel] += coefficient * double(m_history[index][channel]);
        }
        if (++index == m_taps) {
          index = 0;
        }
      }
      const std::array<float, 2> frame{float(sum[0]), float(sum[1])};
      writeStereoFrame(m_output, frame, output.data() + result.outputFrames * m_output.channels);
      ++result.outputFrames;
      m_phase += m_step;
      m_pending = static_cast<std::size_t>(m_phase / m_denominator);
      m_phase %= m_denominator;
    }
    return result;
  }

private:
  static constexpr std::uint64_t phaseCount = 1024;
  PcmFormat m_input, m_output;
  std::size_t m_taps = 0, m_write = 0, m_pending = 0;
  std::uint64_t m_denominator = 1, m_step = 1, m_phase = 0;
  std::vector<float> m_coefficients;
  std::vector<std::array<float, 2>> m_history;
};
} // namespace deskflow::audio::dsp
