/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Real Opus/SRTP/jitter/ASRC/ring with independent virtual source/device clocks.
// No audio endpoint, wall-time waits or sockets. Logical clock is explicit on render.
#include "audio/codec/OpusCodec.h"
#include "audio/engine/AudioEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace deskflow::audio;
using namespace deskflow::audio::engine;
void require(bool ok, const char *message)
{
  if (!ok)
    throw std::runtime_error(message);
}
SrtpKey testKey()
{
  std::array<std::uint8_t, transport::kMasterKeySaltBytes> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>(i + 55);
  return SrtpKey(bytes);
}
void simulate(
    unsigned packetMs, unsigned callbackFrames, double sourcePpm, std::uint32_t ssrc, unsigned deviceRate = 48000,
    long double callbackPhase = .5L, std::uint64_t workerPollNs = 500000, std::uint64_t deliveryQuantumNs = 0,
    std::uint64_t jitterNs = 0
)
{
  constexpr std::uint64_t epoch = 1000000000000ULL;
  constexpr std::uint64_t horizon = 420000000000ULL;
  constexpr std::uint64_t steadyAt = 120000000000ULL;
  const auto frames = static_cast<std::uint16_t>(48 * packetMs);
  const StreamConfig config{ssrc, 1, 48000, 2, frames};
  codec::OpusCodec codec(frames);
  transport::SrtpSender sender(config, testKey());
  AudioReceiver receiver({deviceRate, 2}, packetMs, config, testKey());
  std::vector<float> pcm(static_cast<std::size_t>(frames) * 2, .00001f), output(callbackFrames * 2);
  std::array<std::uint8_t, transport::kMaxPayloadBytes> opus{};
  const auto bytes = codec.encode(pcm, opus);
  require(bytes > 0, "Clock fixture encode failed");
  // Repeat a valid encoded frame; this test measures clock/transport/buffer correctness,
  // not Opus quality/history. Every ciphertext still has a unique SRTP packet index.
  const auto packetPeriod = static_cast<long double>(packetMs) * 1000000 / (1 + sourcePpm / 1e6L);
  const auto callbackPeriod = static_cast<long double>(callbackFrames) * 1e9L / deviceRate;
  std::uint64_t sourceIndex = 0, callbackIndex = 0, nextWorker = epoch;
  const auto deliveryTime = [&](std::uint64_t index) -> long double {
    const auto nominal = epoch + index * packetPeriod;
    const auto variableDelay = jitterNs == 0 ? 0 : ((index * 2654435761ULL) % (jitterNs + 1));
    const auto arrived = static_cast<std::uint64_t>(std::llround(nominal)) + variableDelay;
    // Quantized receive-poll delivery. Packet generation/media clock is unchanged;
    // at most one packet per event here because tested batches are shorter than P.
    if (deliveryQuantumNs == 0)
      return arrived;
    return epoch + ((arrived - epoch + deliveryQuantumNs - 1) / deliveryQuantumNs) * deliveryQuantumNs;
  };
  long double nextSource = deliveryTime(0), nextCallback = epoch + callbackPeriod * callbackPhase;
  ReceiverStats baseline{};
  bool haveBaseline = false;
  double sumCorrection = 0;
  std::size_t observations = 0;
  while (true) {
    const auto next = std::min({nextSource, nextCallback, static_cast<long double>(nextWorker)});
    if (next > epoch + horizon)
      break;
    const auto now = static_cast<std::uint64_t>(std::llround(next));
    if (nextSource <= nextCallback && nextSource <= nextWorker) {
      const auto protectedPacket = sender.protect(
          std::span<const std::uint8_t>(opus.data(), bytes), static_cast<std::uint16_t>(sourceIndex),
          static_cast<std::uint32_t>(sourceIndex * frames)
      );
      require(bool(protectedPacket), "Clock fixture protect failed");
      require(receiver.ingest(protectedPacket.packet, now), "Authenticated clock stream rejected");
      ++sourceIndex;
      nextSource = deliveryTime(sourceIndex);
    } else if (nextCallback <= nextWorker) {
      require(receiver.render(output, now), "Clock fixture render failed");
      ++callbackIndex;
      nextCallback = epoch + callbackPeriod * (static_cast<long double>(callbackIndex) + callbackPhase);
    } else {
      receiver.tick(now, now);
      nextWorker += workerPollNs; // Independent bounded worker poll, not packet-aligned scheduling.
      if (now >= epoch + steadyAt) {
        if (!haveBaseline) {
          baseline = receiver.stats();
          haveBaseline = true;
        }
        sumCorrection += receiver.stats().correctionPpm;
        ++observations;
      }
    }
  }
  const auto final = receiver.stats();
  std::cout << "packet_ms=" << packetMs << " device_rate=" << deviceRate << " callback_frames=" << callbackFrames
            << " callback_phase=" << static_cast<double>(callbackPhase)
            << " worker_poll_us=" << double(workerPollNs) / 1000
            << " delivery_quantum_us=" << double(deliveryQuantumNs) / 1000
            << " delivery_jitter_us=" << double(jitterNs) / 1000 << " source_ppm=" << sourcePpm
            << " mean_correction=" << sumCorrection / double(observations) << " last_correction=" << final.correctionPpm
            << " trim=" << final.playbackTrimmedFrames - baseline.playbackTrimmedFrames
            << " rejected_pcm=" << final.playbackRejectedFrames - baseline.playbackRejectedFrames
            << " underrun=" << final.underrunFrames - baseline.underrunFrames
            << " jitter_lost=" << final.jitterLostPackets << " jitter_late=" << final.jitterLatePackets
            << " feedback_updates=" << final.feedbackUpdates - baseline.feedbackUpdates
            << " feedback_conflicts=" << final.feedbackConflicts - baseline.feedbackConflicts
            << " feedback_invalid=" << final.feedbackInvalid - baseline.feedbackInvalid
            << " feedback_cooldown=" << final.feedbackCooldown - baseline.feedbackCooldown << '\n';
  require(
      !final.requiresReanchor && final.jitterLostPackets == 0 && final.jitterLatePackets == 0,
      "Joint network/source clocks failed"
  );
  require(
      final.playbackTrimmedFrames == baseline.playbackTrimmedFrames &&
          final.playbackRejectedFrames == baseline.playbackRejectedFrames &&
          final.underrunFrames == baseline.underrunFrames,
      "Steady-state playback lost/trimmed frames"
  );
  require(
      std::abs(sumCorrection / double(observations) - sourcePpm) < 1,
      "Phase-normalized integrated clock estimate did not converge"
  );
}
} // namespace
int main(int argc, char **argv)
{
  try {
    const bool extended = argc == 2 && std::string_view(argv[1]) == "--extended";
    const bool network = argc == 2 && std::string_view(argv[1]) == "--network";
    const bool catchup = argc == 2 && std::string_view(argv[1]) == "--catchup";
    if (argc != 1 && !extended && !network && !catchup) {
      std::cerr << "Usage: deskflow-audio-engine-clock [--extended|--network|--catchup]\n";
      return 2;
    }
    if (catchup) {
      // Packet arrival is immediate, but decoder work is polled every 7ms. For
      // 5ms media this necessarily exercises multi-packet tick catch-up. This
      // deliberately probes beyond the previously verified <=2ms worker cadence.
      std::uint32_t ssrc = 0x40010000;
      for (double ppm : {-500.0, 0.0, 500.0})
        simulate(5, 480, ppm, ++ssrc, 48000, .999L, 7000000);
      std::cout << "Catch-up joint engine clock simulation passed (3 cases, worker7ms, P5ms)\n";
    } else if (network) {
      // Deterministic positive-delay network trace + bounded receive-poll quantization.
      // Not packet loss/reordering or a statistical model of a real LAN/Wi-Fi link.
      std::uint32_t ssrc = 0x30010000;
      for (unsigned packetMs : {5U, 10U})
        for (unsigned rate : {44100U, 48000U})
          for (double ppm : {-500.0, 500.0})
            for (std::uint64_t batch : {0ULL, 1000000ULL})
              simulate(packetMs, (rate + 99) / 100, ppm, ++ssrc, rate, .999L, 500000, batch, 1000000);
      std::cout
          << "Network-delivery joint engine clock simulation passed (16 cases, 420s per case, last300s acceptance; "
             "0..1ms deterministic jitter + 0/1ms receive quantum, no hardware/concurrency claim)\n";
    } else if (!extended) {
      // Keep the original thirty-case baseline unchanged. This fixture is public,
      // but every case still needs its own SSRC to avoid SRTP packet-index reuse.
      std::uint32_t ssrc = 0x10010000;
      for (unsigned packetMs : {5U, 10U})
        for (unsigned callbackFrames : {128U, 479U, 480U})
          for (double ppm : {-500.0, -100.0, 0.0, 100.0, 500.0})
            simulate(packetMs, callbackFrames, ppm, ++ssrc);
      std::cout << "Joint engine clock simulation passed (30 cases, 420s per case, last300s acceptance)\n";
    } else {
      // A bounded covering sample, NOT the full Cartesian/pairwise matrix: fully
      // cross rate, packet duration, callback quantum and drift sign; rotate drift
      // magnitude, callback phase and worker cadence to keep this to 48 cases.
      // No network jitter or delivery batching: source events authenticate immediately,
      // independently of decoder polling; no worker can ingest a not-yet-arrived packet.
      constexpr std::array<unsigned, 4> rates{44100, 48000, 96000, 192000};
      constexpr std::array<unsigned, 2> packets{5, 10};
      constexpr std::array<std::uint64_t, 3> quantumUs{2667, 9979, 10000};
      constexpr std::array<long double, 3> phases{0, .001L, .999L};
      constexpr std::array<std::uint64_t, 3> workerPollNs{500000, 1000000, 2000000};
      std::uint32_t ssrc = 0x20010000; // Disjoint from the default thirty-case fixture.
      for (std::size_t rateIndex = 0; rateIndex < rates.size(); ++rateIndex) {
        for (std::size_t packetIndex = 0; packetIndex < packets.size(); ++packetIndex) {
          for (std::size_t quantumIndex = 0; quantumIndex < quantumUs.size(); ++quantumIndex) {
            const auto rate = rates[rateIndex];
            const auto callbackFrames = static_cast<unsigned>((rate * quantumUs[quantumIndex] + 999999) / 1000000);
            const double magnitude = (rateIndex + packetIndex + quantumIndex) % 2 == 0 ? 100 : 500;
            for (std::size_t sign = 0; sign < 2; ++sign) {
              const auto phaseIndex = (rateIndex + packetIndex + quantumIndex + sign) % phases.size();
              const auto pollIndex = (rateIndex + 2 * packetIndex + 2 * quantumIndex + sign) % workerPollNs.size();
              simulate(
                  packets[packetIndex], callbackFrames, sign == 0 ? -magnitude : magnitude, ++ssrc, rate,
                  phases[phaseIndex], workerPollNs[pollIndex]
              );
            }
          }
        }
      }
      std::cout << "Extended joint engine clock simulation passed (48 cases, 420s per case, last300s acceptance; "
                   "no network jitter/batching, no hardware/concurrency claim)\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Engine clock simulation failed: " << error.what() << '\n';
    return 1;
  }
}
