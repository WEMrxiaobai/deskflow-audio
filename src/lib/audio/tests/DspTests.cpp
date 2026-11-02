/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Standalone C++20 test executable; no Qt, gtest, device, codec or network dependencies.
// Example: cl /std:c++20 /EHsc /W4 /I src/lib src/lib/audio/tests/DspTests.cpp /Fe:DspTests.exe
#include "audio/dsp/CapturePipeline.h"
#include "audio/dsp/ClockDrift.h"
#include "audio/dsp/JitterBuffer.h"
#include "audio/dsp/Playback.h"
#include "audio/dsp/PlaybackClock.h"
#include "audio/dsp/SilenceGate.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace deskflow::audio::dsp;

namespace {
std::size_t checks = 0;
void check(bool value, const char *expression, int line)
{
  ++checks;
  if (!value) {
    std::cerr << "FAILED line " << line << ": " << expression << '\n';
    std::exit(1);
  }
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)
bool near(double a, double b, double tolerance = 0.00001)
{
  return std::abs(a - b) <= tolerance;
}
template <class Function> void throws(Function function)
{
  bool caught = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    caught = true;
  }
  CHECK(caught);
}

void pcmTests()
{
  CHECK(PcmFormat{}.samplesForFrames(240) == 480);
  CHECK(PcmFormat{}.framesForDuration(5) == 240);
  throws([] { (void)PcmFormat{44100, 2}.framesForDuration(5); });
  throws([] { (void)SpscPcmQueue({48000, 0}, 3, 240); });
  throws([] { (void)SpscPcmRing({48000, 1}, 0); });
  throws([] { (void)PcmFormat{}.samplesForFrames(std::numeric_limits<std::size_t>::max()); });
  SpscPcmQueue queue({48000, 1}, 2, 3);
  std::array<float, 3> data{1, 2, 3}, output{};
  CHECK(queue.push(data, {0, 100, 0, false, 7}));
  CHECK(queue.push(data, {1, 103, 0, true, 7}));
  CHECK(!queue.push(data, {2}));
  CHECK(queue.queuedBlocks() == 2);
  PcmBlockInfo info;
  CHECK(queue.peek(info));
  CHECK(info.frames == 3 && info.generation == 7 && info.timestampFrames == 100);
  CHECK(!queue.pop(std::span<float>(output.data(), 2), info));
  CHECK(queue.pop(output, info));
  CHECK(output == data && info.sequence == 0);
  CHECK(queue.push(data, {2})); // ring index wraps
  CHECK(queue.discard());
  CHECK(queue.pop(output, info) && info.sequence == 2);
  CHECK(!queue.discard());

  SpscPcmRing ring({48000, 1}, 4);
  CHECK(ring.write(data, true));
  bool discontinuity = false;
  std::array<float, 1> frame{};
  CHECK(ring.readFrame(frame, discontinuity) && discontinuity && frame[0] == 1);
  CHECK(!ring.write(data));
  CHECK(ring.discardTo(1) == 1);
  CHECK(ring.write(data));
  CHECK(ring.queuedFrames() == 4);
  CHECK(ring.readFrame(frame, discontinuity) && !discontinuity && frame[0] == 3);
  CHECK(ring.discardTo(0) == 3);
  CHECK(!ring.readFrame(frame, discontinuity));
}

std::vector<float> adapt(
    PcmFormat inputFormat, PcmFormat outputFormat, const std::vector<float> &input, std::size_t chunkFrames,
    std::size_t outputChunkFrames, double ppm = 0
)
{
  StreamAdapter adapter(inputFormat, outputFormat);
  CHECK(adapter.setRateCorrectionPpm(ppm));
  std::vector<float> result, scratch(outputChunkFrames * outputFormat.channels);
  const auto totalFrames = input.size() / inputFormat.channels;
  for (std::size_t offset = 0; offset < totalFrames;) {
    const auto chunk = std::min(chunkFrames, totalFrames - offset);
    auto source = std::span<const float>(input.data() + offset * inputFormat.channels, chunk * inputFormat.channels);
    for (;;) {
      const auto progress = adapter.process(source, scratch);
      CHECK(progress.valid);
      CHECK(progress.inputFrames <= source.size() / inputFormat.channels);
      CHECK(progress.outputFrames <= outputChunkFrames);
      result.insert(result.end(), scratch.begin(), scratch.begin() + progress.outputFrames * outputFormat.channels);
      source = source.subspan(progress.inputFrames * inputFormat.channels);
      if (progress.inputFrames == 0 && progress.outputFrames == 0) {
        CHECK(source.empty());
        break;
      }
    }
    offset += chunk;
  }
  return result;
}

void adapterTests()
{
  throws([] { (void)StreamAdapter({7999, 2}, {48000, 2}); });
  throws([] { (void)StreamAdapter({48000, 6}, {48000, 2}); });
  StreamAdapter adapter({48000, 1}, {48000, 2});
  CHECK(!adapter.setRateCorrectionPpm(5001));
  CHECK(!adapter.setRateCorrectionPpm(std::numeric_limits<double>::quiet_NaN()));
  std::array<float, 3> input{1, 2, 3};
  std::array<float, 8> out{};
  const auto progress = adapter.process(input, out);
  CHECK(progress.inputFrames == 3 && progress.outputFrames == 3);
  CHECK(out[0] == 1 && out[1] == 1 && out[4] == 3 && out[5] == 3);
  adapter.reset();
  CHECK(adapter.process(input, {}).inputFrames == 0);
  CHECK(!adapter.process(input, std::span<float>(out.data(), 3)).valid);

  const auto mono = adapt({48000, 2}, {48000, 1}, {1, 3, -1, 1, 2, 4}, 1, 1);
  CHECK((mono == std::vector<float>{2, 0, 3}));
  const auto sanitized = adapt(
      {48000, 1}, {48000, 1}, {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 0.01f},
      1, 1
  );
  CHECK(sanitized[0] == 0 && sanitized[1] == 0 && near(sanitized[2], .01));

  // Exact rational phase gives the same output for one large callback and arbitrary
  // fragmented input/output; 44.1/48 kHz must not accumulate callback-rounding drift.
  std::vector<float> ramp(44101);
  for (std::size_t i = 0; i < ramp.size(); ++i) {
    ramp[i] = float(i);
  }
  const auto whole = adapt({44100, 1}, {48000, 1}, ramp, ramp.size(), 1024);
  const auto fragmented = adapt({44100, 1}, {48000, 1}, ramp, 73, 7);
  CHECK(whole == fragmented && whole.size() == 48001);
  for (std::size_t i = 0; i < whole.size(); i += 113) {
    CHECK(near(whole[i], double(i) * 44100 / 48000, .005));
  }
  std::vector<float> longInput(480001, 0.25f);
  const auto corrected = adapt({48000, 1}, {48000, 1}, longInput, 137, 91, 1000);
  CHECK(corrected.size() == 479521); // floor((480000)/1.001)+1
  CHECK(corrected.front() == .25f && corrected.back() == .25f);
  const auto downsampled = adapt({48000, 1}, {8000, 1}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, 1, 1);
  CHECK((downsampled == std::vector<float>{0, 6, 12}));
  // Exercise extreme supported rate ratios, both drift directions, and callback partitions.
  for (auto rate : {8000u, 44100u, 48000u, 96000u, 192000u}) {
    for (double ppm : {-4999.999, 0.0, 4999.999}) {
      std::vector<float> signal(1001);
      for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = float(i) / 1000;
      }
      const auto reference = adapt({rate, 1}, {48000, 1}, signal, signal.size(), 2048, ppm);
      const auto pieces = adapt({rate, 1}, {48000, 1}, signal, 3, 1, ppm);
      CHECK(reference == pieces && !reference.empty());
      for (auto value : pieces) {
        CHECK(std::isfinite(value) && value >= 0 && value <= 1);
      }
    }
  }

  // Updating the ratio must preserve buffered samples and phase, including fractional lookahead.
  StreamAdapter changing({48000, 1}, {48000, 1});
  std::array<float, 1> sample{1}, destination{};
  CHECK(changing.process(sample, destination).outputFrames == 1);
  CHECK(changing.setRateCorrectionPpm(1000));
  sample[0] = 2;
  CHECK(changing.process(sample, destination).outputFrames == 1 && destination[0] == 2);
  CHECK(changing.setRateCorrectionPpm(-1000));
  sample[0] = 3;
  CHECK(changing.process(sample, destination).outputFrames == 0);
  sample[0] = 4;
  CHECK(changing.process(sample, destination).outputFrames == 1 && near(destination[0], 3.001));
}

void surroundTests()
{
  for (const PcmFormat format : {PcmFormat{48000, 6, 0x3f}, PcmFormat{48000, 6, 0x60f},
                                 PcmFormat{48000, 8, 0x63f}}) {
    for (std::size_t channel = 0; channel < format.channels; ++channel) {
      std::vector<float> input(format.channels);
      input[channel] = 1;
      const auto output = adapt(format, {48000, 2}, input, 1, 1);
      CHECK(output.size() == 2);
      if (channel == 2 || channel == 3)
        CHECK(output[0] > 0 && near(output[0], output[1]));
      else if (channel % 2 == 0)
        CHECK(output[0] > 0 && output[1] == 0);
      else
        CHECK(output[1] > 0 && output[0] == 0);
    }
    std::vector<float> full(format.channels, 1.0f);
    const auto mixed = adapt(format, {48000, 2}, full, 1, 1);
    CHECK(near(mixed[0], 1) && near(mixed[1], 1));
    const auto expanded = adapt({48000, 2}, format, {0.25f, -0.5f}, 1, 1);
    CHECK(expanded.size() == format.channels && expanded[0] == .25f && expanded[1] == -.5f);
    for (std::size_t channel = 2; channel < format.channels; ++channel)
      CHECK(expanded[channel] == 0);
    BandLimitedAdapter resample(format, {44100, 2});
    std::vector<float> sustained(format.channels * 256, 1), output(512);
    const auto progress = resample.process(sustained, output);
    CHECK(progress.outputFrames > 100);
    CHECK(near(output[(progress.outputFrames - 1) * 2], 1, .001));
  }
  throws([] { (void)StreamAdapter({48000, 6, 0}, {48000, 2}); });
  throws([] { (void)StreamAdapter({48000, 8, 0xff}, {48000, 2}); });
  throws([] { (void)StreamAdapter({48000, 4, 0x33}, {48000, 2}); });
}

void captureTests()
{
  throws([] { (void)CapturePipeline({48000, 1}, {48000, 1}, 20, 2); });
  CapturePipeline capture({48000, 1}, {48000, 1}, 5, 2, 7, 1000);
  std::vector<float> first(100, .25f), second(380, .5f), block(240), extra(240, .75f);
  CHECK(capture.push(first, false, 1000000));
  CHECK(capture.latestTimestampFrames() == 1100);
  CHECK(capture.queue().queuedBlocks() == 0);
  CHECK(capture.push(second, false, 2000000));
  CHECK(capture.latestTimestampFrames() == 1480);
  CHECK(capture.queue().queuedBlocks() == 2);
  CHECK(capture.push(extra)); // full queue rejects third block, does not block callback
  CHECK(capture.stats().droppedBlocks == 1 && capture.stats().producedBlocks == 2);
  PcmBlockInfo info;
  CHECK(capture.queue().pop(block, info));
  CHECK(info.sequence == 0 && info.timestampFrames == 1000 && info.generation == 7 && info.discontinuity);
  CHECK(info.enqueuedNs == 2000000 && info.capturedNs == 1000000);
  CHECK(block[99] == .25f && block[100] == .5f);
  CHECK(capture.queue().pop(block, info));
  CHECK(info.sequence == 1 && info.timestampFrames == 1240 && !info.discontinuity);
  CHECK(info.enqueuedNs == 2000000);
  CHECK(capture.push(extra));
  CHECK(capture.queue().pop(block, info));
  CHECK(info.sequence == 3 && info.timestampFrames == 1720 && info.discontinuity);

  CHECK(capture.push(first));
  capture.resetStream(8, 0);
  CHECK(capture.stats().droppedPartialFrames == 100);
  CHECK(capture.push(extra));
  capture.resetStream(9, 100);
  CHECK(capture.push(extra));
  CHECK(capture.popForGeneration(block, info, 9, 100, 1000));
  CHECK(info.generation == 9 && info.timestampFrames == 100 && info.discontinuity);
  CHECK(capture.stats().wrongGenerationBlocks == 1);
  CHECK(capture.push(extra));
  CHECK(!capture.popForGeneration(block, info, 9, 1340, 1000)); // timestamp=340, exactly expired
  CHECK(capture.stats().expiredBlocks == 1);

  CapturePipeline ten({44100, 2}, {48000, 2}, 10, 3);
  std::vector<float> converted(441 * 2, .4f), result(480 * 2);
  CHECK(ten.push(converted));
  // One input-frame lookahead may defer the very last interpolated output until next callback.
  std::array<float, 2> continuation{.4f, .4f};
  CHECK(ten.push(continuation));
  CHECK(ten.queue().pop(result, info) && info.frames == 480);
  for (auto value : result) {
    CHECK(near(value, .4));
  }
}

void playbackTests()
{
  PlaybackSource source({48000, 1}, 8, 4, 1);
  std::array<float, 4> ones{1, 1, 1, 1}, output{};
  CHECK(source.write(ones));
  CHECK(source.render(output));
  CHECK(near(output[0], .0625) && output[3] == 1); // initial PCM and volume ramps
  CHECK(source.render(output));
  CHECK(near(output[0], .75) && near(output[1], .5) && near(output[2], .25) && output[3] == 0);
  CHECK(source.render(output));
  CHECK((output == std::array<float, 4>{0, 0, 0, 0})); // never repeat last frame
  CHECK(source.stats().underrunEvents == 1 && source.stats().underrunFrames == 8);
  CHECK(source.write(ones));
  CHECK(source.render(output));
  CHECK(near(output[0], .25) && output[3] == 1);
  std::array<float, 4> minus{-1, -1, -1, -1};
  CHECK(source.write(minus, true));
  CHECK(source.render(output));
  CHECK(near(output[0], .5) && near(output[1], 0) && near(output[2], -.5) && output[3] == -1);
  source.setMuted(true);
  CHECK(source.write(ones));
  CHECK(source.render(output));
  CHECK(output[3] == 0);
  source.resetStream();
  CHECK(source.muted()); // reconnect cannot unmute
  source.setMuted(false);
  source.setVolume(.5f);
  CHECK(source.write(ones));
  CHECK(source.render(output));
  CHECK(near(output[3], .5));
  CHECK(source.write(ones));
  CHECK(source.queuedFramesForWriter() == 4 && source.writableFrames() == 4);
  CHECK(source.write(ones));
  CHECK(source.queuedFramesForWriter() == 8 && source.writableFrames() == 0);
  CHECK(!source.write(ones));
  CHECK(source.stats().rejectedFrames == 4);
  CHECK(source.trimTo(2) == 6 && source.stats().trimmedFrames == 6);

  PlaybackSource timed({48000, 1}, 8, 1, 1);
  CHECK(timed.write(ones, false, 1000000));
  CHECK(timed.render(output, 4000000));
  CHECK(
      timed.stats().residenceFrames == 4 && timed.stats().residenceNsTotal == 12000000 &&
      timed.stats().residenceNsMax == 3000000
  );
  CHECK(timed.render(output, 9000000)); // Underruns do not masquerade as queue residence.
  CHECK(timed.stats().residenceFrames == 4);

  PlaybackSource a({48000, 1}, 8, 1, 1), b({48000, 1}, 8, 1, 1);
  std::array<PlaybackSource *, 2> sources{&a, &b};
  CHECK(a.write(ones) && b.write(ones));
  CHECK(mixSources(sources, {48000, 1}, output));
  CHECK(near(output[0], .98) && near(output[3], .98));
  CHECK(b.write(ones));
  CHECK(mixSources(sources, {48000, 1}, output)); // a underruns independently
  CHECK(near(output[0], .98) && near(output[3], .98));
  b.setMuted(true);
  CHECK(b.write(ones));
  CHECK(mixSources(sources, {48000, 1}, output));
  CHECK(output[3] == 0);
  std::array<float, 4> extreme{
      std::numeric_limits<float>::max(), std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::max()
  };
  CHECK(a.write(extreme));
  CHECK(mixSources(sources, {48000, 1}, output));
  CHECK(near(output[0], .98) && output[1] == 0 && output[2] == 0 && near(output[3], -.98));
  CHECK(!mixSources(sources, {48000, 2}, output));
  CHECK((output == std::array<float, 4>{0, 0, 0, 0}));
}

void jitterTests()
{
  throws([] { (void)JitterBuffer(32768, 100, 240, 5000000); });
  throws([] { (void)JitterBuffer(4, 0, 240, 5000000); });
  JitterBuffer buffer(3, 10, 240, 5000000);
  std::array<std::uint8_t, 2> data{42, 99};
  std::array<std::uint8_t, 10> out{};
  CHECK(buffer.poll(0, out).status == PacketPoll::NotStarted);
  buffer.start(7, 65535, 100, 10000000);
  CHECK(buffer.insert({0, 340, 7, false}, data, 1) == PacketInsert::Accepted); // reordered wrapped seq
  CHECK(buffer.insert({65535, 100, 7, true}, data, 2) == PacketInsert::Accepted);
  CHECK(buffer.insert({0, 340, 7}, data, 2) == PacketInsert::Duplicate);
  CHECK(buffer.insert({1, 580, 6}, data, 2) == PacketInsert::WrongGeneration);
  CHECK(buffer.insert({2, 820, 7}, data, 2) == PacketInsert::OutOfWindow);
  CHECK(buffer.insert({1, 999, 7}, data, 2) == PacketInsert::Invalid);
  CHECK(buffer.poll(9999999, out).status == PacketPoll::Waiting);
  CHECK(buffer.poll(10000000, std::span<std::uint8_t>(out.data(), 1)).status == PacketPoll::DestinationTooSmall);
  CHECK(buffer.queuedPackets() == 2);
  const auto packet = buffer.poll(10000000, out);
  CHECK(
      packet.status == PacketPoll::Packet && packet.info.sequence == 65535 && packet.info.resumed && packet.bytes == 2
  );
  CHECK(out[0] == 42 && out[1] == 99);
  CHECK(buffer.insert({65535, 100, 7}, data, 10000000) == PacketInsert::Late);
  CHECK(buffer.poll(15000000, out).status == PacketPoll::Packet);
  CHECK(buffer.insert({1, 580, 7}, data, 20000000) == PacketInsert::Late); // exact deadline is late
  const auto loss = buffer.poll(20000000, out);
  CHECK(loss.status == PacketPoll::Loss && loss.info.sequence == 1 && loss.info.timestampFrames == 580);
  CHECK(buffer.poll(20000000, out).status == PacketPoll::Waiting);
  CHECK(buffer.stats().lost == 1 && buffer.stats().delivered == 2 && buffer.stats().late == 2);
  CHECK(buffer.insert({2, 820, 7}, data, 20000001) == PacketInsert::Accepted);
  buffer.start(8, 12, 0, 30000000);
  CHECK(buffer.queuedPackets() == 0);
  CHECK(buffer.insert({12, 0, 7}, data, 1) == PacketInsert::WrongGeneration);
  CHECK(buffer.poll(30000000, out).status == PacketPoll::Loss);
  buffer.stop();
  CHECK(buffer.poll(90000000, out).status == PacketPoll::NotStarted);

  // Non-power-of-two slot count must remain correct across multiple sequence wraps.
  JitterBuffer wraps(7, 2, 1, 10);
  wraps.start(1, 65530, 0, 100);
  for (std::uint64_t i = 0; i < 131100; ++i) {
    CHECK(wraps.insert({static_cast<std::uint16_t>(65530 + i), i, 1}, data, 99 + 10 * i) == PacketInsert::Accepted);
    const auto current = wraps.poll(100 + 10 * i, out);
    CHECK(current.status == PacketPoll::Packet && current.info.timestampFrames == i);
  }
}

void silenceTests()
{
  SilenceGate gate({48000, 1}, 4);
  std::array<float, 2> zeros{0, -0.0f};
  CHECK(gate.inspect(zeros).send);
  auto decision = gate.inspect(zeros);
  CHECK(decision.valid && !decision.send && decision.enteredSilence);
  CHECK(!gate.inspect(zeros).enteredSilence);
  std::array<float, 2> low{std::numeric_limits<float>::denorm_min(), 0};
  decision = gate.inspect(low);
  CHECK(decision.send && decision.resumed && !gate.suppressed());
  CHECK(!gate.inspect(low).resumed);
  (void)gate.inspect(zeros);
  (void)gate.inspect(zeros);
  low[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK(gate.inspect(low).resumed);
  CHECK(!gate.inspect({}).valid);
  gate.reset();
  CHECK(!gate.suppressed());
}

void driftTests()
{
  throws([] {
    DriftConfig config;
    config.maxPpm = 6000;
    (void)ClockDriftController(config);
  });
  ClockDriftController controller;
  CHECK(controller.update(480, .1) == 0);
  CHECK(controller.update(1480, .1) == 10); // slew bound
  for (int i = 0; i < 2000; ++i) {
    (void)controller.update(1480, .1);
  }
  CHECK(controller.correctionPpm() <= 1000 && controller.correctionPpm() > 900);
  CHECK(controller.update(-1, .1) == controller.correctionPpm());
  CHECK(controller.update(480, 10) == controller.correctionPpm());
  controller.reset();
  CHECK(controller.update(0, .1) == -10);
  controller.reset();

  // Simulate independent +200ppm sender clock over 10 minutes. Buffer occupancy is
  // bounded by feedback rather than periodic drops, and settles near its target.
  double occupancy = 480;
  double maxOccupancy = 480;
  for (int i = 0; i < 60000; ++i) {
    const auto correction = controller.update(occupancy, .01);
    occupancy += 48000 * .01 * (200 - correction) / 1000000;
    maxOccupancy = std::max(maxOccupancy, occupancy);
    CHECK(occupancy > 0 && occupancy < 960);
  }
  CHECK(maxOccupancy < 600);
  CHECK(std::abs(occupancy - 480) < 1);
  CHECK(std::abs(controller.estimatedDriftPpm() - 200) < 1);
}

void playbackClockTests()
{
  PlaybackClock delayed;
  double pending = 0;
  delayed.begin();
  delayed.end(1000000000, 480, 48000);
  CHECK(delayed.observe(1000000000, 48000, [] { return 0; }, pending));
  delayed.begin();
  delayed.end(1005000000, 480, 48000);
  CHECK(!delayed.observe(1005000000, 48000, [] { return 0; }, pending));
  delayed.begin();
  delayed.end(1021000000, 240, 48000);
  CHECK(delayed.observe(1021000000, 48000, [] { return 0; }, pending));
  CHECK(near(pending, 240));
  delayed.begin();
  delayed.end(1023500000, 120, 48000);
  CHECK(delayed.observe(1023500000, 48000, [] { return 0; }, pending));
  CHECK(near(pending, 240)); // 120 remaining from the prior callback plus 120 new.
  PlaybackClock clock;
  double value = -1;
  CHECK(!clock.observe(1000000000, 48000, [] { return 240; }, value));
  clock.begin();
  bool conflict = false;
  CHECK(!clock.observe(1000000000, 48000, [] { return 240; }, value, &conflict));
  CHECK(conflict);
  clock.end(1000000000, 480);
  CHECK(clock.observe(1005000000, 48000, [] { return 240; }, value));
  CHECK(near(value, 480)); // 240 in ring + 240 submitted but not yet nominally played.
  CHECK(clock.observe(1010000000, 48000, [] { return 240; }, value) && near(value, 240));
  CHECK(!clock.observe(1030000000, 48000, [] { return 240; }, value));
  CHECK(!clock.observe(999000000, 48000, [] { return 240; }, value));
  CHECK(!clock.observe(
      1005000000, 48000,
      [&] {
        clock.begin();
        clock.end(1005000000, 480);
        return 240;
      },
      value
  ));
  clock.begin();
  clock.end(0, 100); // An underrun snapshot cannot steer the controller.
  CHECK(!clock.observe(1005000000, 48000, [] { return 240; }, value, &conflict));
  CHECK(!conflict); // Actual discontinuity resets the baseline; overlap must not.

  PlaybackClock concurrent;
  std::atomic<std::size_t> ringFrames{1000};
  std::atomic<bool> done{false};
  std::thread reader([&] {
    for (std::size_t i = 0; i < 100000; ++i) {
      concurrent.begin();
      const auto consumed = i % 481;
      ringFrames.store(1000 - consumed, std::memory_order_release);
      concurrent.end(1000000000, consumed);
    }
    done.store(true, std::memory_order_release);
  });
  bool consistent = true;
  do {
    if (concurrent.observe(1000000000, 48000, [&] { return ringFrames.load(std::memory_order_acquire); }, value)) {
      consistent = consistent && near(value, 1000);
    }
  } while (!done.load(std::memory_order_acquire));
  reader.join();
  CHECK(consistent);
  CHECK(concurrent.observe(1000000000, 48000, [&] { return ringFrames.load(std::memory_order_acquire); }, value));
  CHECK(near(value, 1000));
}
void concurrentSpscTests()
{
  constexpr std::size_t count = 100000;
  SpscPcmQueue queue({48000, 1}, 7, 1);
  std::atomic<bool> failure{false};
  std::thread producer([&] {
    for (std::size_t i = 0; i < count; ++i) {
      const std::array<float, 1> sample{float(i)};
      while (!queue.push(sample, {i, i, 1, false, 42})) {
        std::this_thread::yield();
      }
    }
  });
  for (std::size_t i = 0; i < count; ++i) {
    std::array<float, 1> sample{};
    PcmBlockInfo info;
    while (!queue.pop(sample, info)) {
      std::this_thread::yield();
    }
    if (sample[0] != float(i) || info.sequence != i || info.generation != 42) {
      failure = true;
    }
  }
  producer.join();
  CHECK(!failure.load());

  SpscPcmRing ring({48000, 1}, 13);
  std::thread ringProducer([&] {
    for (std::size_t i = 0; i < count; ++i) {
      const std::array<float, 1> sample{float(i)};
      while (!ring.write(sample, i % 97 == 0)) {
        std::this_thread::yield();
      }
    }
  });
  for (std::size_t i = 0; i < count; ++i) {
    std::array<float, 1> sample{};
    bool discontinuity = false;
    while (!ring.readFrame(sample, discontinuity)) {
      std::this_thread::yield();
    }
    if (sample[0] != float(i) || discontinuity != (i % 97 == 0)) {
      failure = true;
    }
  }
  ringProducer.join();
  CHECK(!failure.load());
}
} // namespace

int main()
{
  pcmTests();
  adapterTests();
  surroundTests();
  captureTests();
  playbackTests();
  jitterTests();
  silenceTests();
  driftTests();
  playbackClockTests();
  concurrentSpscTests();
  std::cout << "DSP standalone tests passed (" << checks << " checks)\n";
}
