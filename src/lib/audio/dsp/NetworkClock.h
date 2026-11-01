/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Pcm.h"

namespace deskflow::audio::dsp {

// Maps a source's RTP media clock to the receiver's monotonic clock. This is
// independent of the PCM/device occupancy controller: resampling decoded PCM
// cannot correct an accumulating error in compressed-packet deadlines.
//
// Decoder-worker-owned. Observe only AUTHENTICATED packets, before jitter.insert(),
// including packets which insert() will reject as late/out-of-window. The caller
// provides this sequence's deadline extrapolated from the current jitter timeline
// (not simply the next expected packet's deadline). Returned duration changes ONLY
// future deadline increments: never jump the currently pending deadline.
//
// Retain the existing trusted reanchor requirement for suspend/long gaps. Call
// reset() on every initial/trusted anchor; this cannot authorize a new stream.
class NetworkClock final
{
public:
  explicit NetworkClock(std::uint64_t nominalPacketDurationNs, std::uint64_t startupDelayNs = 5000000)
      : m_nominal(nominalPacketDurationNs),
        m_startup(startupDelayNs)
  {
    if (nominalPacketDurationNs == 0 || nominalPacketDurationNs > 1000000000ULL || startupDelayNs > 1000000000ULL) {
      throw std::invalid_argument("Invalid network media clock configuration");
    }
  }

  void reset() noexcept
  {
    m_haveSequence = false;
    m_filteredPhaseSeconds = 0;
    m_integralPpm = 0;
    m_correctionPpm = 0;
  }

  [[nodiscard]] std::uint64_t
  observe(std::uint16_t sequence, std::uint64_t arrivalNs, std::uint64_t expectedDeadlineNs) noexcept
  {
    if (!m_haveSequence) {
      m_haveSequence = true;
      m_sequence = sequence;
      return packetDurationNs();
    }
    const auto delta = static_cast<std::uint16_t>(sequence - m_sequence);
    if (delta == 0 || delta >= 32768) { // Duplicates/reordered packets cannot steer time.
      return packetDurationNs();
    }
    m_sequence = sequence;
    const double dt = double(delta) * double(m_nominal) / 1e9;
    // A long outage is not evidence of oscillator drift. The application must
    // explicitly reanchor it instead of pulling a stale timeline toward new media.
    if (dt > 1.0) {
      return packetDurationNs();
    }
    // Subtract before converting so nanosecond precision is not lost at large
    // monotonic epochs. No unsigned subtraction or addition may wrap here.
    const double arrivalMinusDeadline = arrivalNs >= expectedDeadlineNs ? double(arrivalNs - expectedDeadlineNs)
                                                                        : -double(expectedDeadlineNs - arrivalNs);
    const double rawPhaseSeconds = (arrivalMinusDeadline + double(m_startup)) / 1e9;
    // Gross scheduling/network discontinuities require a trusted reanchor, not
    // a frequency update. This also prevents a single extreme timestamp outlier
    // from poisoning the low-pass filter for seconds after the outlier is gone.
    if (std::abs(rawPhaseSeconds) > 0.040) {
      return packetDurationNs();
    }
    const double phaseSeconds = std::clamp(rawPhaseSeconds, -0.005, 0.005);
    constexpr double filterSeconds = 0.5;
    constexpr double proportionalPerSecond = 0.5;
    constexpr double integralPerSecondSquared = 0.1;
    constexpr double maxClockPpm = 1000;
    constexpr double maxCorrectionPpm = 2000; // Frequency +/-1000 plus phase recovery.
    m_filteredPhaseSeconds += dt / (filterSeconds + dt) * (phaseSeconds - m_filteredPhaseSeconds);
    const double proportionalPpm = m_filteredPhaseSeconds * proportionalPerSecond * 1e6;
    const double proposedIntegral = std::clamp(
        m_integralPpm + m_filteredPhaseSeconds * integralPerSecondSquared * dt * 1e6, -maxClockPpm, maxClockPpm
    );
    const double proposed = proportionalPpm + proposedIntegral;
    if (std::abs(proposed) <= maxCorrectionPpm || (proposed > 0 && m_filteredPhaseSeconds < 0) ||
        (proposed < 0 && m_filteredPhaseSeconds > 0)) {
      m_integralPpm = proposedIntegral;
    }
    m_correctionPpm = std::clamp(proportionalPpm + m_integralPpm, -maxCorrectionPpm, maxCorrectionPpm);
    return packetDurationNs();
  }

  [[nodiscard]] std::uint64_t packetDurationNs() const noexcept
  {
    return static_cast<std::uint64_t>(std::llround(double(m_nominal) * (1.0 + m_correctionPpm / 1e6)));
  }
  // Positive means source packets arrive more slowly than nominal: lengthen the
  // receiver's packet deadlines. PCM resampling has a separate sign convention.
  [[nodiscard]] double correctionPpm() const noexcept
  {
    return m_correctionPpm;
  }
  [[nodiscard]] double estimatedClockPpm() const noexcept
  {
    return m_integralPpm;
  }

private:
  const std::uint64_t m_nominal, m_startup;
  bool m_haveSequence = false;
  std::uint16_t m_sequence = 0;
  double m_filteredPhaseSeconds = 0, m_integralPpm = 0, m_correctionPpm = 0;
};
} // namespace deskflow::audio::dsp
