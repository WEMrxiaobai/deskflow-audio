/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Independent event simulation, not a device or engine integration test.
// Continuous/fractional frame occupancy isolates feedback and bulk-delivery/reader-trim
// interaction from integer ASRC phase, packet jitter PLL, FIR startup, and OS scheduling.
#include "audio/dsp/ClockDrift.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
using deskflow::audio::dsp::ClockDriftController;
using deskflow::audio::dsp::DriftConfig;
struct Result
{
  double finalPpm = 0, meanPpm = 0, minQueue = 1e9, maxQueue = 0;
  double trimmed = 0, rejected = 0, underrun = 0;
  std::uint64_t saturationSamples = 0, samples = 0;
};
// sourcePpm changes producer-clock frequency, device remains the nominal 48k clock.
// Corrections consume decoded input faster, so output packet has P/(1+u) frames.
// Both delivery and callback events are periodic; the delivery worker can coalesce
// P=5ms packets when its polling quantum is 10ms. Every packet separately calls PI,
// exactly as writePcm currently does. Callbacks trim BEFORE consuming a whole block.
Result simulate(
    double packetMs, double callbackMs, double phaseMs, double sourcePpm, double trimMs, double deliveryQuantumMs = 0.0,
    double seconds = 600.0, double targetMs = 5.0, double capacityMs = 20.0, double kp = .5
)
{
  constexpr double rate = 48000.0;
  const double capacity = rate * capacityMs / 1000.0, target = rate * targetMs / 1000.0;
  const double p = packetMs / 1000.0, c = callbackMs / 1000.0;
  const double sourcePeriod = p / (1.0 + sourcePpm * 1e-6);
  const double horizon = seconds;
  ClockDriftController controller(DriftConfig{target, kp, .1});
  double queue = target, producedAt = 0.0, deliverAt = 0.0, callbackAt = phaseMs / 1000.0;
  double now = 0.0;
  Result result;
  while (now < horizon) {
    // Producer/decoder first on exact ties; phase cases deliberately exercise both sides.
    if (deliverAt <= callbackAt) {
      now = deliverAt;
      while (producedAt <= now + 1e-10) {
        const auto correction = controller.update(queue, p);
        const auto output = packetMs * 48.0 / (1.0 + correction * 1e-6);
        if (queue + output <= capacity)
          queue += output;
        else if (now > horizon / 2)
          result.rejected += output;
        producedAt += sourcePeriod;
        if (now > horizon / 2) {
          result.finalPpm = correction;
          result.meanPpm += correction;
          ++result.samples;
          if (std::abs(correction) >= 999.0)
            ++result.saturationSamples;
        }
      }
      if (deliveryQuantumMs > 0)
        deliverAt += deliveryQuantumMs / 1000.0;
      else
        deliverAt = producedAt;
    } else {
      now = callbackAt;
      const auto threshold = rate * trimMs / 1000.0;
      if (queue > threshold) {
        if (now > horizon / 2)
          result.trimmed += queue - threshold;
        queue = threshold;
      }
      const auto needed = callbackMs * 48.0;
      if (now > horizon / 2 && queue < needed)
        result.underrun += needed - queue;
      queue = std::max(0.0, queue - needed);
      callbackAt += c;
    }
    if (now > horizon / 2) {
      result.minQueue = std::min(result.minQueue, queue);
      result.maxQueue = std::max(result.maxQueue, queue);
    }
  }
  if (result.samples)
    result.meanPpm /= static_cast<double>(result.samples);
  return result;
}
void print(
    double p, double c, double phase, double drift, double trim, double batch, const Result &r, double targetMs = 5.0,
    double capacityMs = 20.0, double kp = .5
)
{
  std::cout << p << ',' << c << ',' << phase << ',' << drift << ',' << trim << ',' << batch << ',' << r.finalPpm << ','
            << r.meanPpm << ',' << r.trimmed << ',' << r.rejected << ',' << r.underrun << ',' << r.minQueue << ','
            << r.maxQueue << ',' << r.saturationSamples << ',' << r.samples << ',' << targetMs << ',' << capacityMs
            << ',' << kp << '\n';
}
void require(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main()
{
  try {
    const auto old = simulate(10, 10, 5, 0, 10);
    require(old.finalPpm < -990 && old.trimmed > 1000, "Old trim/target pathology was not reproduced");
    const auto raised = simulate(10, 10, 5, 0, 15);
    require(
        std::abs(raised.finalPpm) < 100 && raised.saturationSamples == 0 && raised.trimmed < 1,
        "Raised trim did not restore equal-clock ten-ms equilibrium"
    );
    // This diagnostic succeeds only when it reproduces BOTH the original pathology and
    // the remaining candidate failure. It is intentionally not a candidate acceptance test.
    const auto candidateFailure = simulate(10, 10, 5, -100, 25, 0, 600, 6, 30, 3);
    require(
        candidateFailure.underrun > 100 && std::abs(candidateFailure.meanPpm + 100) > 10,
        "Expected candidate phase-bias failure changed; inspect the model before changing acceptance claims"
    );
    std::cout << std::setprecision(10);
    std::cout << "packet_ms,callback_ms,callback_phase_ms,source_ppm,trim_ms,delivery_quantum_ms,"
                 "final_ppm,mean_ppm_last_half,trimmed_frames_last_half,rejected_frames_last_half,"
                 "underrun_frames_last_half,min_queue_last_half,max_queue_last_half,saturation_samples,samples,"
                 "target_ms,capacity_ms,kp\n";
    for (const auto trim : {10.0, 15.0})
      for (const auto p : {5.0, 10.0})
        for (const auto c : {2.5, 5.0, 10.0})
          for (const auto phaseFraction : {.001, .25, .5, .999})
            for (const auto ppm : {-100.0, 0.0, 100.0})
              for (const auto batch : {0.0, 10.0}) {
                const auto phase = c * phaseFraction;
                const auto result = simulate(p, c, phase, ppm, trim, batch);
                print(p, c, phase, ppm, trim, batch, result);
              }
    for (const auto p : {5.0, 10.0})
      for (const auto c : {128.0 / 48.0, 5.0, 10.0, 4.113})
        for (const auto phaseFraction : {.001, .25, .5, .999})
          for (const auto ppm : {-1000.0, -500.0, -100.0, 0.0, 100.0, 500.0, 1000.0})
            for (const auto batch : {0.0, 10.0}) {
              const auto phase = c * phaseFraction;
              const auto result = simulate(p, c, phase, ppm, 25, batch, 600, 6, 30, 3);
              print(p, c, phase, ppm, 25, batch, result, 6, 30, 3);
            }
    // Deliberately unsupported 20ms callbacks: a 15ms trim cannot feed a 20ms callback.
    const auto unsupported = simulate(5, 20, 10, 0, 15);
    require(unsupported.underrun > 1000, "Unsupported callback-quantum limit was not observed");
    print(5, 20, 10, 0, 15, 0, unsupported);
    std::cerr << "Playback clock DIAGNOSTIC reproduced old pathology AND candidate failure; NOT candidate acceptance. "
                 "Fractional occupancy excludes FIR/integer rounding, PLL, loss, OS and hardware.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Playback clock event-model failure: " << error.what() << '\n';
    return 1;
  }
}
