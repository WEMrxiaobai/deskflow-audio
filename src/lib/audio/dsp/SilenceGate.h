/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Pcm.h"

namespace deskflow::audio::dsp {
struct SilenceDecision
{
  bool valid = false;
  bool send = false;
  bool resumed = false;
  bool enteredSilence = false;
};

// Exact-zero optimization only, never VAD/noise gating. +/-0 count as zero;
// ANY nonzero including subnormal, NaN or infinity must pass. Inspect before optional
// nonfinite PCM sanitization if preserving that distinction matters. No network keepalive
// responsibility. On resumed=true, encoder/receiver must signal/reset stale media state.
class SilenceGate
{
public:
  SilenceGate(PcmFormat format, std::uint64_t zeroFramesBeforeSuppress)
      : m_format(format),
        m_threshold(zeroFramesBeforeSuppress)
  {
    if (!format.valid() || zeroFramesBeforeSuppress == 0) {
      throw std::invalid_argument("SilenceGate needs a format and positive threshold");
    }
  }
  [[nodiscard]] SilenceDecision inspect(std::span<const float> pcm) noexcept
  {
    if (pcm.empty() || !m_format.wholeFrames(pcm)) {
      return {};
    }
    const bool exactZeros = std::all_of(pcm.begin(), pcm.end(), [](float sample) { return sample == 0.0f; });
    if (!exactZeros) {
      const auto resumed = m_suppressed;
      reset();
      return {true, true, resumed, false};
    }
    const auto frames = pcm.size() / m_format.channels;
    m_zeros += std::min<std::uint64_t>(frames, m_threshold - m_zeros);
    if (m_zeros >= m_threshold) {
      const bool entered = !m_suppressed;
      m_suppressed = true;
      return {true, false, false, entered};
    }
    return {true, true, false, false};
  }
  void reset() noexcept
  {
    m_zeros = 0;
    m_suppressed = false;
  }
  [[nodiscard]] bool suppressed() const noexcept
  {
    return m_suppressed;
  }

private:
  PcmFormat m_format;
  std::uint64_t m_threshold, m_zeros = 0;
  bool m_suppressed = false;
};
} // namespace deskflow::audio::dsp
