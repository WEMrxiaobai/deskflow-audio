/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "Pcm.h"

namespace deskflow::audio::dsp {
// One device reader publishes, one decoder worker observes. Not a physical audio clock:
// assumes the callback's PCM starts playing at its entry time at the nominal device rate.
// Actual WASAPI padding/IAudioClock support can replace this approximation later.
// Snapshot is single-attempt, never a seqlock spin. All fields are atomic: an overlapping
// callback cannot create a C++ data race. Sequence/fields are seq_cst for simple ordering.
class PlaybackClock final
{
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
  void begin() noexcept
  {
    m_sequence.fetch_add(1);
  }
  void end(std::uint64_t callbackNs, std::size_t consumedFrames, std::uint32_t rate = 0) noexcept
  {
    // A delayed device wake can lead to several bounded submissions before
    // earlier submissions have played. Carry their nominal unplayed frames;
    // reject the snapshot if the total exceeds the model's 10 ms bound.
    bool tooMuchPending = false;
    if (rate && callbackNs) {
      if (m_lastNs && callbackNs >= m_lastNs) {
        const double elapsed = double(callbackNs - m_lastNs) * double(rate) / 1e9;
        m_outstanding = std::max(0.0, m_outstanding - elapsed);
      } else {
        m_outstanding = 0;
      }
      m_outstanding += double(consumedFrames);
      m_lastNs = callbackNs;
      tooMuchPending = m_outstanding > double((rate + 99u) / 100u) + 0.5;
      consumedFrames = tooMuchPending ? 0 : static_cast<std::size_t>(std::ceil(m_outstanding));
    } else if (rate) {
      m_lastNs = 0;
      m_outstanding = 0;
    }
    m_callbackNs.store(tooMuchPending ? 0 : callbackNs);
    m_consumed.store(consumedFrames);
    m_sequence.fetch_add(1);
  }
  template <class QueueSnapshot>
  [[nodiscard]] bool observe(
      std::uint64_t nowNs, std::uint32_t rate, QueueSnapshot &&queueSnapshot, double &queuedAndPending,
      bool *transientConflict = nullptr
  ) const noexcept
  {
    if (transientConflict)
      *transientConflict = false;
    const auto before = m_sequence.load();
    if ((before & 1) != 0) {
      if (transientConflict)
        *transientConflict = true;
      return false;
    }
    if (before == 0)
      return false;
    const auto callbackNs = m_callbackNs.load();
    const auto consumed = m_consumed.load();
    const auto queued = queueSnapshot();
    const auto after = m_sequence.load();
    if (before != after || nowNs < callbackNs) {
      if (transientConflict)
        *transientConflict = true;
      return false;
    }
    if (callbackNs == 0)
      return false;
    const auto elapsedNs = nowNs - callbackNs;
    // Old callback evidence is not a stalled-device frequency measurement.
    if (elapsedNs > 20000000ULL)
      return false;
    const auto elapsedFrames = double(elapsedNs) * double(rate) / 1e9;
    queuedAndPending = double(queued) + std::max(0.0, double(consumed) - elapsedFrames);
    return true;
  }

private:
  std::atomic<std::uint64_t> m_sequence{0}, m_callbackNs{0};
  std::atomic<std::size_t> m_consumed{0};
  std::uint64_t m_lastNs = 0; // Writer only.
  double m_outstanding = 0;
};
} // namespace deskflow::audio::dsp
