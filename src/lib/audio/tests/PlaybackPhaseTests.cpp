/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Independent event/counting model. Only the production ClockDrift controller is reused.
// No engine, FIR, packet-clock PLL, audio device, network, OS scheduling, or external DSP.
// A success exit means the finite idealized matrix below passed, not hardware acceptance.
// Build from VS x64 prompt:
// cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /wd4324 /I src/lib ^
//   src/lib/audio/tests/PlaybackPhaseTests.cpp /Fe:build/tmp/PlaybackPhaseTests.exe ^
//   /Fo:build/tmp/PlaybackPhaseTests.obj
#include "audio/dsp/ClockDrift.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using deskflow::audio::dsp::ClockDriftController;
using deskflow::audio::dsp::DriftConfig;
constexpr long double rate = 48000;
constexpr long double endSeconds = 1200;
constexpr long double steadyStart = 900;
constexpr std::uint64_t capacityFrames = 1440; // 30ms
constexpr std::uint64_t trimFrames = 1200;     // 25ms
struct Config
{
  unsigned packetMs = 5, callbackFrames = 128;
  long double phaseFraction = 0;
  double sourcePpm = 0, targetMs = 12;
  bool phaseCompensated = true, warm = true, producerFirst = true;
  bool skipUnderrunSnapshot = false;
};
struct Window
{
  std::uint64_t trim = 0, rejected = 0, underrun = 0, producerSamples = 0, saturationSamples = 0;
  std::uint64_t feedbackUpdates = 0, feedbackSkipped = 0;
  double correctionSum = 0, correctionMin = 1e9, correctionMax = -1e9, correctionEnd = 0;
  double metricSum = 0, metricMin = 1e9, metricMax = -1e9;
  std::uint64_t ringMin = capacityFrames, ringMax = 0;
};
struct Result
{
  Window startup, steady;
};
void observe(Window &window, double correction, double metric, std::uint64_t ring)
{
  ++window.producerSamples;
  if (std::abs(correction) >= 999.99)
    ++window.saturationSamples;
  window.correctionSum += correction;
  window.correctionMin = std::min(window.correctionMin, correction);
  window.correctionMax = std::max(window.correctionMax, correction);
  window.correctionEnd = correction;
  window.metricSum += metric;
  window.metricMin = std::min(window.metricMin, metric);
  window.metricMax = std::max(window.metricMax, metric);
  window.ringMin = std::min(window.ringMin, ring);
  window.ringMax = std::max(window.ringMax, ring);
}
Result simulate(const Config &config)
{
  const long double nominalPeriod = static_cast<long double>(config.packetMs) / 1000;
  const long double sourcePeriod = nominalPeriod / (1 + static_cast<long double>(config.sourcePpm) / 1e6L);
  const long double callbackPeriod = static_cast<long double>(config.callbackFrames) / rate;
  const long double phase = callbackPeriod * config.phaseFraction;
  const auto targetFrames = static_cast<double>(rate * config.targetMs / 1000);
  ClockDriftController controller(DriftConfig{targetFrames, 3, .1, 1000, 100});
  // Warm initial state represents a real preceding callback. Its remaining PCM and ring
  // add to target at t=0; it is NOT an imaginary C frames of padding added on each observe.
  long double lastCallbackTime = phase - callbackPeriod;
  long double actualCallbackPcm = config.warm ? config.callbackFrames : 0;
  long double pendingAtZero = config.warm ? std::max(0.0L, actualCallbackPcm + rate * lastCallbackTime) : 0;
  std::uint64_t ring =
      config.warm ? static_cast<std::uint64_t>(std::max(0.0L, static_cast<long double>(targetFrames) - pendingAtZero))
                  : 0;
  long double fractionalOutput = 0;
  bool callbackSnapshotValid = config.warm;
  std::uint64_t producerIndex = 0, readerIndex = 0;
  long double producerAt = 0, readerAt = phase;
  Result result;
  while (std::min(producerAt, readerAt) < endSeconds) {
    const bool sameTime = std::abs(producerAt - readerAt) < 1e-12L;
    const bool producer = sameTime ? config.producerFirst : producerAt < readerAt;
    const long double now = producer ? producerAt : readerAt;
    Window &window = now >= steadyStart ? result.steady : result.startup;
    if (producer) {
      // Real audio is at the head of a callback block; any zero-filled underrun tail
      // is excluded. A new callback below replaces the prior modeled device block.
      const long double elapsed = std::max(0.0L, now - lastCallbackTime);
      const long double pending = std::max(0.0L, actualCallbackPcm - rate * elapsed);
      const double metric = static_cast<double>(ring) + (config.phaseCompensated ? static_cast<double>(pending) : 0);
      const bool canUpdate = !config.skipUnderrunSnapshot || callbackSnapshotValid;
      const auto correction =
          canUpdate ? controller.update(metric, static_cast<double>(sourcePeriod)) : controller.correctionPpm();
      if (canUpdate)
        ++window.feedbackUpdates;
      else
        ++window.feedbackSkipped;
      // Integer output frame production with carried phase, not a fluid occupancy ring.
      fractionalOutput += rate * nominalPeriod / (1 + static_cast<long double>(correction) / 1e6L);
      const auto frames = static_cast<std::uint64_t>(std::floor(fractionalOutput));
      fractionalOutput -= frames;
      if (frames <= capacityFrames - ring)
        ring += frames;
      else
        window.rejected += frames; // all-or-nothing write, exactly like the bounded ring contract
      observe(window, correction, metric, ring);
      producerAt = static_cast<long double>(++producerIndex) * sourcePeriod;
    } else {
      if (ring > trimFrames) {
        window.trim += ring - trimFrames;
        ring = trimFrames;
      }
      const auto actual = std::min<std::uint64_t>(ring, config.callbackFrames);
      window.underrun += config.callbackFrames - actual;
      ring -= actual;
      actualCallbackPcm = static_cast<long double>(actual);
      callbackSnapshotValid = actual == config.callbackFrames;
      lastCallbackTime = now;
      readerAt = phase + static_cast<long double>(++readerIndex) * callbackPeriod;
    }
  }
  return result;
}
double meanCorrection(const Window &w)
{
  return w.correctionSum / static_cast<double>(w.producerSamples);
}
bool noLoss(const Window &w)
{
  return w.trim == 0 && w.rejected == 0 && w.underrun == 0;
}
void print(const Config &c, const Result &r)
{
  const auto &w = r.steady;
  std::cout << (c.phaseCompensated ? "ring_plus_actual_pending" : "ring_only") << ',' << c.packetMs << ','
            << c.callbackFrames << ',' << static_cast<double>(c.phaseFraction) << ',' << c.sourcePpm << ','
            << c.targetMs << ',' << c.warm << ',' << c.producerFirst << ',' << r.startup.underrun << ','
            << r.startup.trim << ',' << r.startup.rejected << ',' << w.underrun << ',' << w.trim << ',' << w.rejected
            << ',' << meanCorrection(w) << ',' << w.correctionMin << ',' << w.correctionMax << ',' << w.correctionEnd
            << ',' << w.metricSum / static_cast<double>(w.producerSamples) << ',' << w.metricMin << ',' << w.metricMax
            << ',' << w.ringMin << ',' << w.ringMax << ',' << w.saturationSamples << ',' << c.skipUnderrunSnapshot
            << ',' << r.startup.feedbackUpdates << ',' << r.startup.feedbackSkipped << ',' << w.feedbackUpdates << ','
            << w.feedbackSkipped << '\n';
}
void require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
int main()
{
  try {
    std::cout << std::setprecision(12);
    std::cout
        << "metric,packet_ms,callback_frames,phase_fraction,source_ppm,target_ms,warm,producer_first,"
           "startup_underrun,startup_trim,startup_rejected,steady_underrun,steady_trim,steady_rejected,"
           "mean_correction,min_correction,max_correction,end_correction,mean_metric_frames,min_metric,max_metric,"
           "min_postwrite_ring,max_postwrite_ring,saturation_samples,skip_underrun_snapshot,"
           "startup_feedback_updates,startup_feedback_skipped,steady_feedback_updates,steady_feedback_skipped\n";
    unsigned failures = 0, cases = 0, boundaryFailures = 0, boundaryCases = 0;
    for (unsigned packetMs : {5U, 10U})
      for (unsigned callbackFrames : {128U, 240U, 441U, 479U, 480U})
        for (long double phase : {0.0L, .25L, .5L, .999L})
          for (double ppm : {-500.0, -100.0, 0.0, 100.0, 500.0, -1000.0, 1000.0})
            for (bool warm : {true, false})
              for (bool first : {true, false}) {
                const Config c{packetMs, callbackFrames, phase, ppm, 12, true, warm, first};
                const auto result = simulate(c);
                print(c, result);
                const bool boundary = std::abs(ppm) >= 1000;
                const bool passed = noLoss(result.steady) && std::abs(meanCorrection(result.steady) - ppm) < 1.0;
                if (boundary) {
                  ++boundaryCases;
                  if (!passed)
                    ++boundaryFailures;
                } else {
                  ++cases;
                  if (!passed)
                    ++failures;
                }
              }
    // The engine may publish no phase snapshot after an underrun. Verify that a full
    // callback eventually arrives from an empty ring, feedback resumes, and the loop
    // does not remain permanently invalid. Preserve the all-actual-pending matrix above.
    unsigned recoveryCases = 0, recoveryFailures = 0;
    for (unsigned packetMs : {5U, 10U})
      for (unsigned callbackFrames : {128U, 240U, 441U, 479U, 480U})
        for (long double phase : {0.0L, .25L, .5L, .999L})
          for (double ppm : {-500.0, -100.0, 0.0, 100.0, 500.0})
            for (bool first : {true, false}) {
              Config c{packetMs, callbackFrames, phase, ppm, 12, true, false, first};
              c.skipUnderrunSnapshot = true;
              const auto result = simulate(c);
              print(c, result);
              ++recoveryCases;
              if (!noLoss(result.steady) || std::abs(meanCorrection(result.steady) - ppm) >= 1.0 ||
                  result.steady.feedbackUpdates == 0 || result.steady.feedbackSkipped != 0)
                ++recoveryFailures;
            }
    // Ring-only negative control: the phase beat is observable even with ample capacity.
    const Config old{10, 480, .5L, -100, 6, false, true, true};
    const auto oldResult = simulate(old);
    print(old, oldResult);
    require(
        oldResult.steady.underrun > 0 || std::abs(meanCorrection(oldResult.steady) + 100) > 5,
        "Negative control failed to expose the old phase-biased ring-only observation"
    );
    // Lower target is intentionally tested rather than silently assumed safe.
    const Config smallTarget{10, 480, .5L, -100, 6, true, true, true};
    const auto smallResult = simulate(smallTarget);
    print(smallTarget, smallResult);
    std::cerr << "Phase-metric ideal event/counting matrix: " << cases << " nonboundary cases, " << failures
              << " failed; " << boundaryCases << " max-correction boundary cases, " << boundaryFailures << " failed.\n"
              << "Invalid-underrun-snapshot empty-startup recovery: " << recoveryCases << " cases, " << recoveryFailures
              << " failed.\n"
              << "Target6 counterexample steady underrun=" << smallResult.steady.underrun
              << "; target12 uses actual audio pending (zero padding excluded). Duration1200s, steady last300s.\n"
              << "Does NOT model FIR, network PLL, jitter, callback late execution, hardware padding measurement, or "
                 "engine concurrency.\n";
    require(
        smallResult.steady.underrun > 0, "Target6 negative control unexpectedly avoided the callback reserve failure"
    );
    require(failures == 0, "Phase-normalized metric candidate failed a nonboundary ideal matrix case; inspect CSV");
    require(recoveryFailures == 0, "Underrun snapshot invalidation did not recover; inspect CSV");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Playback phase model failed: " << error.what() << '\n';
    return 1;
  }
}
