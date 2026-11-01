/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Pcm.h"

namespace deskflow::audio::dsp {
struct DriftConfig
{
  double targetFrames = 480;
  double proportionalPpmPerFrame = 0.5;
  double integralPpmPerFrameSecond = 0.1;
  double maxPpm = 1000;
  double slewPpmPerSecond = 100;
};

// Per-source bounded occupancy PI controller, not a nominal sample-rate converter.
// Call on a worker, not every sample. Positive correction consumes decoded input faster.
// Feed update() results to StreamAdapter::setRateCorrectionPpm(). Nominal input/output
// rates remain configured separately in that adapter. Integral feedback estimates persistent
// clock offset; short jitter is moderated by proportional gain and slew limits. No packet
// drops are used as clock correction. Reset after stream generation changes or silence gaps.
class ClockDriftController
{
public:
  explicit ClockDriftController(DriftConfig config = {}) : m_config(config)
  {
    if (!std::isfinite(config.targetFrames) || config.targetFrames < 0 ||
        !std::isfinite(config.proportionalPpmPerFrame) || config.proportionalPpmPerFrame < 0 ||
        !std::isfinite(config.integralPpmPerFrameSecond) || config.integralPpmPerFrameSecond < 0 ||
        !std::isfinite(config.maxPpm) || config.maxPpm <= 0 || config.maxPpm > 5000 ||
        !std::isfinite(config.slewPpmPerSecond) || config.slewPpmPerSecond <= 0) {
      throw std::invalid_argument("Invalid clock drift controller configuration");
    }
  }
  // Invalid occupancy/time leaves state unchanged. dt is seconds on the local playback
  // clock; >1s is rejected because it represents a scheduling/stream gap, not feedback.
  [[nodiscard]] double update(double queuedInputFrames, double dtSeconds) noexcept
  {
    if (!std::isfinite(queuedInputFrames) || queuedInputFrames < 0 || !std::isfinite(dtSeconds) || dtSeconds <= 0 ||
        dtSeconds > 1) {
      return m_correction;
    }
    const auto error = queuedInputFrames - m_config.targetFrames;
    const auto proportional = error * m_config.proportionalPpmPerFrame;
    const auto proposedIntegral = std::clamp(
        m_integral + error * m_config.integralPpmPerFrameSecond * dtSeconds, -m_config.maxPpm, m_config.maxPpm
    );
    const auto proposed = proportional + proposedIntegral;
    // Conditional integration anti-windup: allow only errors steering away from saturation.
    if (std::abs(proposed) <= m_config.maxPpm || (proposed > 0 && error < 0) || (proposed < 0 && error > 0)) {
      m_integral = proposedIntegral;
    }
    const auto target = std::clamp(proportional + m_integral, -m_config.maxPpm, m_config.maxPpm);
    const auto maxChange = m_config.slewPpmPerSecond * dtSeconds;
    m_correction += std::clamp(target - m_correction, -maxChange, maxChange);
    return m_correction;
  }
  void reset() noexcept
  {
    m_integral = m_correction = 0;
  }
  [[nodiscard]] double correctionPpm() const noexcept
  {
    return m_correction;
  }
  [[nodiscard]] double estimatedDriftPpm() const noexcept
  {
    return m_integral;
  }

private:
  DriftConfig m_config;
  double m_integral = 0, m_correction = 0;
};
} // namespace deskflow::audio::dsp
