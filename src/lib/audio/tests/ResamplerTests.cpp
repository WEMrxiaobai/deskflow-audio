/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Standalone, standard-library C++20; no Qt/gtest/codecs/devices or external DSP code.
// From a VS x64 developer prompt (create build/tmp first):
// set TEMP=%CD%\build\tmp
// set TMP=%TEMP%
// cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /wd4324 /I src/lib src/lib/audio/tests/ResamplerTests.cpp
// /Fe:build/tmp/ResamplerTests.exe /Fo:build/tmp/ResamplerTests.obj build\tmp\ResamplerTests.exe
#include "audio/dsp/BandLimitedAdapter.h"

#include <cstdlib>
#include <iostream>
#include <new>
#ifdef _MSC_VER
#include <malloc.h>
#endif

// Count every C++ allocation, including aligned and nothrow forms, while the realtime guard
// is active. Audio callbacks are run only after inputs, outputs and adapters are constructed.
namespace {
bool countAllocations = false;
std::size_t allocations = 0;
void *allocate(std::size_t size)
{
  if (countAllocations) {
    ++allocations;
  }
  if (auto *p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}
void *allocateAligned(std::size_t size, std::size_t alignment)
{
  if (countAllocations) {
    ++allocations;
  }
#ifdef _MSC_VER
  if (auto *p = _aligned_malloc(size == 0 ? 1 : size, alignment)) {
    return p;
  }
#else
  const auto rounded = ((std::max(size, std::size_t(1)) + alignment - 1) / alignment) * alignment;
  if (auto *p = std::aligned_alloc(alignment, rounded)) {
    return p;
  }
#endif
  throw std::bad_alloc();
}
void freeAligned(void *p) noexcept
{
#ifdef _MSC_VER
  _aligned_free(p);
#else
  std::free(p);
#endif
}
} // namespace
void *operator new(std::size_t n)
{
  return allocate(n);
}
void *operator new[](std::size_t n)
{
  return allocate(n);
}
void operator delete(void *p) noexcept
{
  std::free(p);
}
void operator delete[](void *p) noexcept
{
  std::free(p);
}
void operator delete(void *p, std::size_t) noexcept
{
  std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept
{
  std::free(p);
}
void *operator new(std::size_t n, std::align_val_t a)
{
  return allocateAligned(n, std::size_t(a));
}
void *operator new[](std::size_t n, std::align_val_t a)
{
  return allocateAligned(n, std::size_t(a));
}
void operator delete(void *p, std::align_val_t) noexcept
{
  freeAligned(p);
}
void operator delete[](void *p, std::align_val_t) noexcept
{
  freeAligned(p);
}
void operator delete(void *p, std::size_t, std::align_val_t) noexcept
{
  freeAligned(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept
{
  freeAligned(p);
}
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p, const std::nothrow_t &) noexcept
{
  std::free(p);
}
void operator delete[](void *p, const std::nothrow_t &) noexcept
{
  std::free(p);
}
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
  try {
    return allocateAligned(n, std::size_t(a));
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
  try {
    return allocateAligned(n, std::size_t(a));
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
  freeAligned(p);
}
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
  freeAligned(p);
}

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

template <class Adapter = BandLimitedAdapter>
std::vector<float> convert(
    PcmFormat source, PcmFormat destination, const std::vector<float> &input, std::size_t inputChunk,
    std::size_t outputChunk, double ppm = 0
)
{
  Adapter adapter(source, destination);
  CHECK(adapter.setRateCorrectionPpm(ppm));
  std::vector<float> output, scratch(outputChunk * destination.channels);
  const auto frames = input.size() / source.channels;
  for (std::size_t offset = 0; offset < frames;) {
    const auto chunk = std::min(inputChunk, frames - offset);
    auto span = std::span<const float>(input).subspan(offset * source.channels, chunk * source.channels);
    for (;;) {
      const auto progress = adapter.process(span, scratch);
      CHECK(progress.valid);
      CHECK(progress.inputFrames <= span.size() / source.channels);
      CHECK(progress.outputFrames <= outputChunk);
      output.insert(output.end(), scratch.begin(), scratch.begin() + progress.outputFrames * destination.channels);
      span = span.subspan(progress.inputFrames * source.channels);
      if (progress.inputFrames == 0 && progress.outputFrames == 0) {
        CHECK(span.empty());
        break;
      }
    }
    offset += chunk;
  }
  return output;
}

std::vector<float> tone(std::uint32_t rate, double frequency, double seconds = 0.25)
{
  std::vector<float> samples(static_cast<std::size_t>(double(rate) * seconds));
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * double(i) / rate + 0.37));
  }
  return samples;
}
// RMS is also meaningful for non-coherent windows and ppm-shifted signals. Skip both startup
// and ending regions; no tail padding is requested from either resampler.
double rms(const std::vector<float> &values)
{
  CHECK(values.size() > 2048);
  double sum = 0;
  for (std::size_t i = 1024; i < values.size() - 1024; ++i) {
    sum += double(values[i]) * values[i];
  }
  return std::sqrt(sum / double(values.size() - 2048));
}
double db(double value)
{
  return 20.0 * std::log10(std::max(value, 1e-20));
}
// Coherent amplitude projection explicitly checks the alias/image frequency, not just RMS.
double amplitude(const std::vector<float> &values, double rate, double frequency)
{
  const auto count = std::min(std::size_t(8192), values.size() - 1024);
  double real = 0, imaginary = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const double angle = 2.0 * std::numbers::pi * frequency * double(i + 1024) / rate;
    real += double(values[i + 1024]) * std::cos(angle);
    imaginary += double(values[i + 1024]) * std::sin(angle);
  }
  return 2.0 * std::hypot(real, imaginary) / double(count);
}

void frequencyTests()
{
  for (auto rate : {96000u, 192000u}) {
    const auto source = tone(rate, 30000);
    const auto filtered = convert({rate, 1}, {48000, 1}, source, 117, 19);
    const auto baseline = convert<StreamAdapter>({rate, 1}, {48000, 1}, source, 117, 19);
    const double filteredAlias = amplitude(filtered, 48000, 18000);
    const double baselineAlias = amplitude(baseline, 48000, 18000);
    const double rejection = db(filteredAlias / baselineAlias);
    std::cout << rate << "->48000 30kHz alias vs linear: " << rejection << " dB\n";
    CHECK(rejection < -60);
    CHECK(db(rms(filtered) / rms(baseline)) < -60);
    for (double ppm : {-5000.0, 0.0, 5000.0}) {
      for (double frequency : {1000.0, 10000.0, 20000.0, 24000.0, 30000.0, 40000.0}) {
        const auto signal = tone(rate, frequency);
        const auto result = convert({rate, 1}, {48000, 1}, signal, 1009, 257, ppm);
        const double gain = db(rms(result) * std::sqrt(2.0));
        std::cout << rate << "->48000 " << frequency << "Hz ppm=" << ppm << ": " << gain << " dB\n";
        if (frequency <= 20000) {
          CHECK(std::abs(gain) < 0.15);
        } else {
          CHECK(gain < -60);
        }
      }
    }
  }
  for (double frequency : {1000.0, 10000.0, 18000.0}) {
    const auto result = convert({44100, 1}, {48000, 1}, tone(44100, frequency), 73, 17);
    const double gain = db(rms(result) * std::sqrt(2.0));
    std::cout << "44100->48000 " << frequency << "Hz passband: " << gain << " dB\n";
    CHECK(std::abs(gain) < 0.15);
  }
  // 8->48 image of a 1 kHz tone at 7 kHz: unlike sample-linear upsampling, the
  // fractional-delay FIR removes imaging. Use a longer coherent measurement window.
  const auto lowRate = tone(8000, 1000, 1.0);
  const auto upsampled = convert({8000, 1}, {48000, 1}, lowRate, 37, 127);
  const auto linear = convert<StreamAdapter>({8000, 1}, {48000, 1}, lowRate, 37, 127);
  const double image = amplitude(upsampled, 48000, 7000);
  const double linearImage = amplitude(linear, 48000, 7000);
  std::cout << "8000->48000 7kHz image: " << db(image) << " dBFS; linear " << db(linearImage) << " dBFS\n";
  CHECK(db(image) < -60);
  CHECK(db(image / linearImage) < -30);
}

void streamingTests()
{
  for (auto sourceRate : {8000u, 44100u, 48000u, 96000u, 192000u}) {
    for (auto destinationRate : {8000u, 48000u, 192000u}) {
      for (double ppm : {-5000.0, 0.0, 5000.0}) {
        const auto signal = tone(sourceRate, 1700, 0.015);
        const auto whole = convert({sourceRate, 1}, {destinationRate, 1}, signal, signal.size(), 4096, ppm);
        const auto pieces = convert({sourceRate, 1}, {destinationRate, 1}, signal, 3, 1, ppm);
        CHECK(whole == pieces && !whole.empty());
        const BandLimitedAdapter adapter({sourceRate, 1}, {destinationRate, 1});
        const auto available = signal.size() - adapter.latencyInputFrames() - 1;
        const auto denominator = std::uint64_t(destinationRate) * 1000000000ULL;
        const auto step =
            std::uint64_t(sourceRate) *
            static_cast<std::uint64_t>(1000000000LL + static_cast<std::int64_t>(std::llround(ppm * 1000.0)));
        // floor(t) <= available, i.e. k*step < (available+1)*denominator.
        const auto expected = ((available + 1) * denominator - 1) / step + 1;
        CHECK(whole.size() == expected);
      }
    }
  }
  const auto mono = tone(44100, 1000);
  std::vector<float> stereo(mono.size() * 2);
  for (std::size_t i = 0; i < mono.size(); ++i) {
    stereo[2 * i] = mono[i];
    stereo[2 * i + 1] = -mono[i];
  }
  const auto duplicated = convert({44100, 1}, {48000, 2}, mono, 71, 11);
  const auto one = convert({44100, 1}, {48000, 1}, mono, 71, 11);
  const auto two = convert({44100, 2}, {48000, 2}, stereo, 71, 11);
  const auto averaged = convert({44100, 2}, {48000, 1}, stereo, 71, 11);
  CHECK(duplicated.size() == 2 * one.size() && two.size() == duplicated.size());
  for (std::size_t i = 0; i < one.size(); ++i) {
    CHECK(duplicated[2 * i] == one[i] && duplicated[2 * i + 1] == one[i]);
    CHECK(two[2 * i] == one[i] && two[2 * i + 1] == -one[i]);
    CHECK(averaged[i] == 0);
  }
}

void stateTests()
{
  for (auto invalid : {PcmFormat{7999, 1}, PcmFormat{192001, 2}, PcmFormat{48000, 0}, PcmFormat{48000, 3}}) {
    bool caught = false;
    try {
      BandLimitedAdapter adapter(invalid, {48000, 1});
    } catch (const std::invalid_argument &) {
      caught = true;
    }
    CHECK(caught);
    caught = false;
    try {
      BandLimitedAdapter adapter({48000, 1}, invalid);
    } catch (const std::invalid_argument &) {
      caught = true;
    }
    CHECK(caught);
  }
  BandLimitedAdapter adapter({96000, 2}, {48000, 2});
  CHECK((adapter.inputFormat() == PcmFormat{96000, 2}));
  CHECK((adapter.outputFormat() == PcmFormat{48000, 2}));
  CHECK(adapter.latencyInputFrames() == 64);
  CHECK((BandLimitedAdapter({192000, 1}, {48000, 1}).latencyInputFrames() == 128));
  std::vector<float> input(4096, .25f), output(4096), replay(4096);
  CHECK(adapter.process(input, {}).inputFrames == 0);
  CHECK(!adapter.process(std::span<const float>(input).first(3), output).valid);
  CHECK(!adapter.process(input, std::span<float>(output).first(3)).valid);
  const auto initial = adapter.process(input, std::span<float>(output).first(2));
  CHECK(initial.inputFrames == 65 && initial.outputFrames == 1); // bounded initial lookahead
  const auto drained = adapter.process({}, output);
  CHECK(drained.inputFrames == 0 && drained.outputFrames == 0); // no fabricated tail
  adapter.reset();
  const auto original = adapter.process(input, output);
  adapter.reset();
  const auto reset = adapter.process(input, replay);
  CHECK(original.inputFrames == reset.inputFrames && original.outputFrames == reset.outputFrames);
  CHECK(output == replay);
  for (std::size_t i = 256; i < original.outputFrames * 2; ++i) {
    CHECK(std::abs(output[i] - .25f) < 1e-6f);
  }

  // Invalid ppm must preserve the current step/state; valid dynamic updates must preserve
  // buffered history/phase. Compare two identical streams with updates at identical output
  // boundaries, one offered large input and one only one stereo frame at a time.
  BandLimitedAdapter large({44100, 2}, {48000, 2}), fragmented({44100, 2}, {48000, 2});
  std::size_t largeOffset = 0, smallOffset = 0;
  for (std::size_t frame = 0; frame < 1000; ++frame) {
    const double ppm = frame % 3 == 0 ? -5000.0 : frame % 3 == 1 ? 5000.0 : 0.0;
    CHECK(large.setRateCorrectionPpm(ppm) && fragmented.setRateCorrectionPpm(ppm));
    CHECK(!fragmented.setRateCorrectionPpm(5000.001));
    CHECK(!fragmented.setRateCorrectionPpm(-5000.001));
    CHECK(!fragmented.setRateCorrectionPpm(std::numeric_limits<double>::quiet_NaN()));
    CHECK(!fragmented.setRateCorrectionPpm(std::numeric_limits<double>::infinity()));
    std::array<float, 2> a{}, b{};
    const auto p = large.process(std::span<const float>(input).subspan(largeOffset * 2), a);
    largeOffset += p.inputFrames;
    CHECK(p.outputFrames == 1);
    for (;;) {
      const auto q = fragmented.process(std::span<const float>(input).subspan(smallOffset * 2, 2), b);
      smallOffset += q.inputFrames;
      if (q.outputFrames == 1) {
        break;
      }
      CHECK(q.inputFrames == 1);
    }
    CHECK(a == b && largeOffset == smallOffset);
  }

  // Start and end semantics: impulse's center output is produced only after exact lookahead,
  // then empty input does not extend the stream with zeros or repeat a final frame.
  BandLimitedAdapter impulse({48000, 1}, {48000, 1});
  std::vector<float> zeros(impulse.latencyInputFrames() + 1);
  zeros[0] = 1;
  std::array<float, 4> values{};
  auto progress = impulse.process(std::span<const float>(zeros).first(zeros.size() - 1), values);
  CHECK(progress.outputFrames == 0 && progress.inputFrames == zeros.size() - 1);
  progress = impulse.process(std::span<const float>(zeros).last(1), values);
  CHECK(progress.outputFrames == 1 && values[0] > .8f && values[0] < 1.0f);
  CHECK(impulse.process({}, values).outputFrames == 0);
  std::vector<float> dirty(2048, std::numeric_limits<float>::quiet_NaN());
  dirty[100] = std::numeric_limits<float>::infinity();
  const auto clean = convert({48000, 1}, {48000, 1}, dirty, 9, 3);
  for (auto value : clean) {
    CHECK(value == 0);
  }
}

// Independent double-precision analytic FIR oracle: this does not consult the stored phase
// table. Measures actual coefficient quantization + phase interpolation error, including
// non-table-aligned phases and near-Nyquist input, rather than merely asserting table size.
void phaseAccuracyTests()
{
  double peakError = 0;
  for (auto rate : {44100u, 192000u}) {
    constexpr double ppm = 1234.567;
    const auto input = tone(rate, rate == 44100 ? 18000 : 20000);
    const auto output = convert({rate, 1}, {48000, 1}, input, 103, 17, ppm);
    const BandLimitedAdapter adapter({rate, 1}, {48000, 1});
    const auto half = adapter.latencyInputFrames();
    const auto taps = half * 2;
    const double cutoff = .91 / std::max(1.0, double(rate) / 48000 * 1.005);
    const std::uint64_t step = std::uint64_t(rate) * 1001234567ULL;
    constexpr std::uint64_t denominator = 48000ULL * 1000000000ULL;
    for (std::size_t frame = 1024; frame < output.size() - 1024; frame += 13) {
      const auto position = std::uint64_t(frame) * step;
      const auto center = static_cast<std::size_t>(position / denominator);
      const double fraction = double(position % denominator) / double(denominator);
      double sum = 0, normalization = 0;
      for (std::size_t tap = 0; tap < taps; ++tap) {
        const double x = double(tap) - double(half) + 1.0 - fraction;
        const double angle = std::numbers::pi * cutoff * x;
        const double sinc = std::abs(angle) < 1e-15 ? 1.0 : std::sin(angle) / angle;
        const double window = std::abs(x) >= double(half)
                                  ? 0
                                  : .42 + .5 * std::cos(std::numbers::pi * x / double(half)) +
                                        .08 * std::cos(2.0 * std::numbers::pi * x / double(half));
        const double coefficient = cutoff * sinc * window;
        normalization += coefficient;
        sum += coefficient * double(input[center - half + 1 + tap]);
      }
      peakError = std::max(peakError, std::abs(sum / normalization - double(output[frame])));
    }
  }
  std::cout << "1024-phase interpolation + float coefficient peak error: " << peakError << '\n';
  CHECK(peakError < 1e-6);
}

void allocationTests()
{
  BandLimitedAdapter adapter({192000, 2}, {8000, 2}); // Largest supported FIR/history.
  std::array<float, 4096> input{}, output{};
  const auto before = allocations;
  countAllocations = true;
  bool valid = true;
  for (std::size_t i = 0; i < 100; ++i) {
    valid = adapter.setRateCorrectionPpm(i % 2 == 0 ? 5000 : -5000) && valid;
    valid = !adapter.setRateCorrectionPpm(5001) && valid;
    const auto p = adapter.process(input, output);
    valid = p.valid && p.inputFrames > 0 && p.outputFrames > 0 && valid;
    adapter.reset();
  }
  countAllocations = false;
  CHECK(valid);
  CHECK(allocations == before);
  std::cout << "process/setRateCorrectionPpm/reset allocations: " << allocations - before << '\n';
}
} // namespace

int main()
{
  stateTests();
  streamingTests();
  frequencyTests();
  phaseAccuracyTests();
  allocationTests();
  std::cout << "ResamplerTests: " << checks << " checks passed\n";
}
