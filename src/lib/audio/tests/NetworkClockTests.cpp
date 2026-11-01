/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Standalone deterministic simulation: no codec, crypto, device, network, or engine.
#include "audio/dsp/JitterBuffer.h"
#include "audio/dsp/NetworkClock.h"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace deskflow::audio::dsp;
namespace {
void check(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
void simulate(std::uint64_t nominal, double sourcePpm, bool noisy)
{
  constexpr std::uint64_t startup = 5000000;
  constexpr std::uint64_t epoch = 1000000000000ULL;
  NetworkClock clock(nominal, startup);
  JitterBuffer jitter(8, 2, nominal == 5000000 ? 240 : 480, nominal);
  constexpr std::uint16_t initialSequence = 65500;
  const std::uint32_t frames = nominal == 5000000 ? 240 : 480;
  jitter.start(1, initialSequence, 0, epoch + startup);
  std::array<std::uint8_t, 2> payload{1, 2}, output{};
  std::uint64_t rejected = 0, lost = 0, total = 600000000000ULL / nominal;
  std::uint64_t random = 7, maxResidence = 0;
  for (std::uint64_t i = 0; i < total; ++i) {
    random = random * 6364136223846793005ULL + 1;
    // Stationary bounded scheduling jitter +/-1ms around a fixed path delay.
    const auto noise = noisy ? static_cast<std::int64_t>((random >> 32) % 2000001) - 1000000 : 0;
    const auto base =
        epoch + static_cast<std::uint64_t>(std::llround(double(i) * double(nominal) * (1.0 + sourcePpm / 1e6)));
    const auto arrival =
        noise < 0 ? base - static_cast<std::uint64_t>(-noise) : base + static_cast<std::uint64_t>(noise);
    for (;;) {
      const auto result = jitter.poll(arrival, output);
      if (result.status == PacketPoll::Waiting)
        break;
      if (result.status == PacketPoll::Loss)
        ++lost;
    }
    const auto sequence = static_cast<std::uint16_t>(initialSequence + i);
    const auto expected = jitter.deadlineFor(sequence);
    check(jitter.setPacketDurationNs(clock.observe(sequence, arrival, expected)), "duration is inside allowed bounds");
    if (jitter.insert({sequence, i * frames, 1}, payload, arrival) != PacketInsert::Accepted)
      ++rejected;
    if (expected >= arrival)
      maxResidence = std::max(maxResidence, expected - arrival);
    // A reordered authenticated duplicate must not affect the oscillator estimate.
    const auto before = clock.packetDurationNs();
    check(
        clock.observe(static_cast<std::uint16_t>(sequence - 1), arrival, expected) == before,
        "reordered packet cannot steer clock"
    );
  }
  std::cout << "packet_ns=" << nominal << " source_ppm=" << sourcePpm << " noisy=" << noisy
            << " correction_ppm=" << clock.correctionPpm() << " estimated_ppm=" << clock.estimatedClockPpm()
            << " rejected=" << rejected << " lost=" << lost << " max_residence_ns=" << maxResidence << '\n';
  check(rejected == 0 && lost == 0, "bounded source drift must not permanently lose media");
  check(maxResidence < 10000000, "clock tracking must not accumulate packet latency");
  check(
      std::abs(clock.estimatedClockPpm() - sourcePpm) < (noisy ? 100 : 1), "PLL integral converges to oscillator offset"
  );
}
} // namespace
int main()
{
  try {
    for (auto nominal : {5000000ULL, 10000000ULL}) {
      for (double ppm : {-1000.0, -500.0, -100.0, 0.0, 100.0, 500.0, 1000.0}) {
        simulate(nominal, ppm, false);
        simulate(nominal, ppm, true);
      }
    }
    NetworkClock clock(5000000);
    check(clock.observe(1, 1000000000, 1005000000) == 5000000, "initial sample does not steer");
    check(clock.observe(2, 9000000000, 1010000000) == 5000000, "long scheduling gap is not frequency evidence");
    clock.reset();
    check(clock.correctionPpm() == 0 && clock.estimatedClockPpm() == 0, "trusted anchor resets PLL");
    std::cout << "Network clock tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
