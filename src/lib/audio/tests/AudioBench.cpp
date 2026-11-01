/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Explicit offline microbenchmark. No audio endpoints, sockets, recording, or runtime permissions.
#include "audio/codec/OpusCodec.h"
#include "audio/dsp/BandLimitedAdapter.h"
#include "audio/dsp/Playback.h"
#include "audio/transport/SrtpTransport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace deskflow::audio;
using Clock = std::chrono::steady_clock;
volatile double observableResult = 0;
struct Distribution
{
  double mean = 0, p95 = 0, p99 = 0, p999 = 0, maximum = 0;
};
Distribution summarize(std::vector<double> &samples)
{
  Distribution result;
  for (auto sample : samples)
    result.mean += sample;
  result.mean /= static_cast<double>(samples.size());
  std::sort(samples.begin(), samples.end());
  const auto quantile = [&](double q) {
    const auto position = q * static_cast<double>(samples.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, samples.size() - 1);
    return samples[lower] + (samples[upper] - samples[lower]) * (position - static_cast<double>(lower));
  };
  result.p95 = quantile(.95);
  result.p99 = quantile(.99);
  result.p999 = quantile(.999);
  result.maximum = samples.back();
  return result;
}
void report(const std::string &name, std::vector<double> &samples, double budgetUs, bool &first)
{
  const auto stats = summarize(samples);
  if (!first)
    std::cout << ",\n";
  first = false;
  std::cout << "    {\"name\":\"" << name << "\",\"iterations\":" << samples.size()
            << ",\"nominal_media_period_us\":" << budgetUs << ",\"mean_us\":" << stats.mean
            << ",\"p95_us\":" << stats.p95 << ",\"p99_us\":" << stats.p99 << ",\"p99_9_us\":" << stats.p999
            << ",\"max_us\":" << stats.maximum << ",\"p99_fraction_of_media_period\":" << stats.p99 / budgetUs << "}";
}
template <class F> void timed(double &destination, F &&operation)
{
  const auto start = Clock::now();
  operation();
  destination = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
void require(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
void fillTone(std::span<float> samples, dsp::PcmFormat format)
{
  for (std::size_t frame = 0; frame < samples.size() / format.channels; ++frame)
    for (std::size_t channel = 0; channel < format.channels; ++channel)
      samples[frame * format.channels + channel] =
          .05f *
          static_cast<float>(std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(frame) / format.sampleRate));
}
transport::SrtpKey testKey()
{
  // Public benchmark-only test material; never sent to a socket or used by the application.
  std::array<std::uint8_t, transport::kMasterKeySaltBytes> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>(i + 1);
  return transport::SrtpKey(bytes);
}
void codecBench(std::size_t iterations, std::uint16_t milliseconds, bool &first)
{
  const auto frames = static_cast<std::uint16_t>(48 * milliseconds);
  codec::OpusCodec opus(frames);
  const transport::StreamConfig config{0x12345000U + milliseconds, 1, 48000, 2, frames};
  transport::SrtpSender sender(config, testKey());
  transport::SrtpReceiver receiver(config, testKey());
  std::vector<float> input(static_cast<std::size_t>(frames) * 2), output(input.size());
  fillTone(input, {48000, 2});
  std::array<std::uint8_t, transport::kMaxPayloadBytes> encoded{};
  std::vector<double> encode(iterations), protect(iterations), unprotect(iterations), decode(iterations);
  std::uint64_t totalBytes = 0;
  constexpr std::size_t warmup = 100;
  for (std::size_t index = 0; index < iterations + warmup; ++index) {
    int count = 0;
    double encodeUs = 0, protectUs = 0, unprotectUs = 0, decodeUs = 0;
    timed(encodeUs, [&] { count = opus.encode(input, encoded); });
    require(count > 0, "Opus encode failed");
    transport::ProtectResult protectedPacket;
    timed(protectUs, [&] {
      protectedPacket = sender.protect(
          std::span<const std::uint8_t>(encoded.data(), count), static_cast<std::uint16_t>(index),
          static_cast<std::uint32_t>(index * frames)
      );
    });
    require(bool(protectedPacket), "SRTP protect failed");
    transport::ReceiveResult received;
    timed(unprotectUs, [&] { received = receiver.unprotect(protectedPacket.packet); });
    require(bool(received), "SRTP unprotect failed");
    int decoded = 0;
    timed(decodeUs, [&] { decoded = opus.decode(received.payload, output); });
    require(decoded == frames, "Opus decode failed");
    observableResult = output.front();
    if (index >= warmup) {
      const auto slot = index - warmup;
      encode[slot] = encodeUs;
      protect[slot] = protectUs;
      unprotect[slot] = unprotectUs;
      decode[slot] = decodeUs;
      totalBytes += protectedPacket.packet.size();
    }
  }
  const auto tag = std::to_string(milliseconds) + "ms";
  report("opus_encode_" + tag, encode, milliseconds * 1000., first);
  report("srtp_protect_" + tag, protect, milliseconds * 1000., first);
  report("srtp_unprotect_" + tag, unprotect, milliseconds * 1000., first);
  report("opus_decode_" + tag, decode, milliseconds * 1000., first);
  // RTP + auth tag included; excludes UDP/IP/Ethernet overhead. Synthetic tone is not music bandwidth.
  std::cout << ",\n    {\"name\":\"tone_srtp_bandwidth_" << tag << "\",\"bits_per_second\":"
            << static_cast<double>(totalBytes) * 8 * 1000 / (static_cast<double>(iterations) * milliseconds)
            << ",\"opus_lookahead_frames\":" << opus.lookaheadFrames() << "}";
}
void adapterBench(std::size_t iterations, dsp::PcmFormat inputFormat, dsp::PcmFormat outputFormat, bool &first)
{
  dsp::BandLimitedAdapter adapter(inputFormat, outputFormat);
  const auto frames = (static_cast<std::size_t>(inputFormat.sampleRate) * 5 + 999) / 1000;
  std::vector<float> input(frames * inputFormat.channels);
  std::vector<float> output((static_cast<std::size_t>(outputFormat.sampleRate) / 100 + 64) * outputFormat.channels);
  std::vector<double> times(iterations);
  fillTone(input, inputFormat);
  for (std::size_t index = 0; index < iterations + 100; ++index) {
    double elapsed = 0;
    timed(elapsed, [&] {
      // Include ppm sweep updates (and the wrap jump); this is not the controller's slew trajectory.
      (void)adapter.setRateCorrectionPpm(static_cast<double>(index % 2001) - 1000.0);
      auto remaining = std::span<const float>(input);
      for (;;) {
        const auto progress = adapter.process(remaining, output);
        remaining = remaining.subspan(progress.inputFrames * inputFormat.channels);
        if (progress.outputFrames != 0)
          observableResult = output.front();
        if (progress.inputFrames == 0 && progress.outputFrames == 0)
          break;
      }
    });
    if (index >= 100)
      times[index - 100] = elapsed;
  }
  report(
      "fir_" + std::to_string(inputFormat.sampleRate) + "_" + std::to_string(outputFormat.sampleRate) + "_" +
          std::to_string(inputFormat.channels) + "ch_to_" + std::to_string(outputFormat.channels) + "ch",
      times, static_cast<double>(frames) * 1000000 / inputFormat.sampleRate, first
  );
}
void playbackBench(std::size_t iterations, bool &first)
{
  const dsp::PcmFormat format{48000, 2};
  dsp::PlaybackSource a(format, 960, 240, .5f), b(format, 960, 240, .5f);
  const std::array<dsp::PlaybackSource *, 2> sources{&a, &b};
  std::array<float, 480> input{}, output{};
  fillTone(input, format);
  std::vector<double> times(iterations);
  for (std::size_t index = 0; index < iterations + 100; ++index) {
    require(a.write(input, false, 1000000) && b.write(input, false, 1000000), "Mix ring unexpectedly full");
    double elapsed = 0;
    timed(elapsed, [&] { require(dsp::mixSources(sources, format, output, .98f, 6000000), "Mix failed"); });
    observableResult = output.front();
    if (index >= 100)
      times[index - 100] = elapsed;
  }
  report("mix_two_sources_5ms_with_residence_telemetry", times, 5000, first);
}
} // namespace
int main(int argc, char **argv)
{
  try {
    std::size_t iterations = 2000;
    if (argc == 2) {
      const std::string value(argv[1]);
      require(
          !value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
          "Iterations must contain decimal digits only"
      );
      std::size_t used = 0;
      iterations = static_cast<std::size_t>(std::stoul(value, &used));
      require(used == value.size(), "Iterations must be an integer");
    }
    require(
        argc <= 2 && iterations >= 200 && iterations <= 100000, "Usage: deskflow-audio-bench [200..100000 iterations]"
    );
    bool first = true;
    std::cout << "{\n  \"schema_version\":1,\n  \"measurement\":\"offline_serial_microbenchmark\",\n"
                 "  \"warning\":\"Not device callback scheduling, CPU utilization, physical latency, quality, or a "
                 "realtime guarantee. "
                 "Record CPU, power plan, compiler, build config and concurrent load separately. No audio device or "
                 "network I/O.\",\n"
                 "  \"warmup_iterations_per_case\":100,\n  \"results\":[\n";
    codecBench(iterations, 5, first);
    codecBench(iterations, 10, first);
    for (const auto rate : {32000U, 44100U, 48000U, 96000U, 192000U}) {
      for (const std::uint8_t channels : {std::uint8_t(1), std::uint8_t(2)}) {
        adapterBench(iterations, {rate, channels}, {48000, 2}, first);
        if (rate != 48000 || channels != 2)
          adapterBench(iterations, {48000, 2}, {rate, channels}, first);
      }
    }
    playbackBench(iterations, first);
    std::cout << "\n  ]\n}\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Audio benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
