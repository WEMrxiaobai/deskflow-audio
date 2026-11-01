// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Standalone tests: no Qt, DSP, codec, CMake integration, or third-party library.
#include "../WasapiBackend.h"
#include "../WasapiPcm.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <objbase.h>
#include <thread>

using namespace deskflow::audio::windows;
using namespace deskflow::audio::windows::detail;
namespace {
int failures = 0;
#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                                \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (false)

WAVEFORMATEX basic(WORD tag, WORD bits)
{
  WAVEFORMATEX f{};
  f.wFormatTag = tag;
  f.nChannels = 2;
  f.nSamplesPerSec = 48000;
  f.wBitsPerSample = bits;
  f.nBlockAlign = static_cast<WORD>(2 * bits / 8);
  f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
  return f;
}

void formats()
{
  PcmLayout layout;
  auto f = basic(WAVE_FORMAT_IEEE_FLOAT, 32);
  CHECK(parseFormat(f, layout) && layout.type == SampleType::Float32);
  f = basic(WAVE_FORMAT_PCM, 16);
  CHECK(parseFormat(f, layout) && layout.type == SampleType::Int16);
  f = basic(WAVE_FORMAT_PCM, 32);
  CHECK(parseFormat(f, layout) && layout.type == SampleType::Int32);
  f = basic(WAVE_FORMAT_PCM, 24);
  CHECK(!parseFormat(f, layout));
  f = basic(WAVE_FORMAT_IEEE_FLOAT, 64);
  CHECK(!parseFormat(f, layout));
  f = basic(WAVE_FORMAT_PCM, 16);
  f.nBlockAlign = 2;
  CHECK(!parseFormat(f, layout));
  f = basic(WAVE_FORMAT_PCM, 16);
  f.nAvgBytesPerSec = 123;
  CHECK(!parseFormat(f, layout));
  f = basic(WAVE_FORMAT_PCM, 16);
  f.nChannels = 0;
  CHECK(!parseFormat(f, layout));

  WAVEFORMATEXTENSIBLE ext{};
  ext.Format = basic(WAVE_FORMAT_EXTENSIBLE, 32);
  ext.Format.cbSize = sizeof(ext) - sizeof(ext.Format);
  ext.Samples.wValidBitsPerSample = 24;
  ext.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  ext.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  CHECK(parseFormat(ext.Format, layout));
  CHECK(layout.validBits == 24 && layout.channelMask == 3);
  ext.Samples.wValidBitsPerSample = 0;
  CHECK(!parseFormat(ext.Format, layout));
  ext.Samples.wValidBitsPerSample = 33;
  CHECK(!parseFormat(ext.Format, layout));
  ext.Samples.wValidBitsPerSample = 24;
  ext.dwChannelMask = SPEAKER_FRONT_LEFT;
  CHECK(!parseFormat(ext.Format, layout));
  ext.dwChannelMask = 0;
  CHECK(parseFormat(ext.Format, layout));
  ext.SubFormat = GUID_NULL;
  CHECK(!parseFormat(ext.Format, layout));
  ext.Format.cbSize = 0;
  CHECK(!parseFormat(ext.Format, layout));

  CHECK(shortestPeriod(4, 48, 448) == 48);
  CHECK(shortestPeriod(4, 49, 448) == 52);
  CHECK(shortestPeriod(4, 49, 51) == 0);
  CHECK(shortestPeriod(0, 48, 448) == 0);
  CHECK(shortestPeriod(4, 0, 448) == 0);
  CHECK(shortestPeriod(4, 500, 448) == 0);
  CHECK(shortestPeriod(0xffffffffu, 0xffffffffu, 0xffffffffu) == 0xffffffffu);
  CHECK(shortestPeriod(0xfffffffeu, 0xffffffffu, 0xffffffffu) == 0);
}

void conversions()
{
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  constexpr float inf = std::numeric_limits<float>::infinity();
  const std::array<float, 11> input{-2, -1, -0.5f, 0, 0.5f, 1, 2, nan, inf, -inf, 0.25f};
  std::array<float, 11> decoded;
  std::array<BYTE, 44> bytes{};
  PcmLayout layout;
  layout.type = SampleType::Int16;
  layout.validBits = 16;
  layout.sampleBytes = 2;
  encodePcm(input, bytes.data(), layout);
  decodePcm(bytes.data(), decoded, layout);
  CHECK(decoded[0] == -1 && decoded[1] == -1 && decoded[2] == -0.5f);
  CHECK(decoded[3] == 0 && decoded[4] == 0.5f);
  CHECK(decoded[5] == 32767.0f / 32768.0f && decoded[6] == decoded[5]);
  CHECK(decoded[7] == 0 && decoded[8] == 0 && decoded[9] == 0 && decoded[10] == 0.25f);

  layout.type = SampleType::Int32;
  layout.validBits = 32;
  layout.sampleBytes = 4;
  encodePcm(input, bytes.data(), layout);
  std::int32_t fullScale = 0;
  std::memcpy(&fullScale, bytes.data() + 5 * 4, 4);
  CHECK(fullScale == std::numeric_limits<std::int32_t>::max());
  decodePcm(bytes.data(), decoded, layout);
  CHECK(decoded[0] == -1 && decoded[2] == -0.5f && decoded[4] == 0.5f && decoded[10] == 0.25f);
  CHECK(decoded[7] == 0 && decoded[8] == 0 && decoded[9] == 0);
  layout.validBits = 24;
  encodePcm(input, bytes.data(), layout);
  for (std::size_t i = 0; i < input.size(); ++i)
    CHECK(bytes[i * 4] == 0);
  decodePcm(bytes.data(), decoded, layout);
  CHECK(decoded[0] == -1 && decoded[2] == -0.5f && decoded[4] == 0.5f);

  layout.type = SampleType::Float32;
  layout.validBits = 32;
  encodePcm(input, bytes.data(), layout);
  decodePcm(bytes.data(), decoded, layout);
  CHECK(decoded[0] == -1 && decoded[5] == 1 && decoded[6] == 1);
  CHECK(decoded[7] == 0 && decoded[8] == 0 && decoded[9] == 0);
  // Capture float values are not clipped, only sanitized.
  std::memcpy(bytes.data(), input.data(), bytes.size());
  decodePcm(bytes.data(), decoded, layout);
  CHECK(decoded[0] == -2 && decoded[6] == 2 && decoded[7] == 0);
  // Null/silent capture cannot dereference an invalid silent packet pointer.
  decodePcm(nullptr, decoded, layout);
  for (const float value : decoded)
    CHECK(value == 0);
}

struct Probe
{
  std::atomic<unsigned> audioCalls{0}, running{0}, retries{0}, stopped{0}, errors{0};
  std::atomic<std::uint64_t> generation{0};
  std::atomic<long> lastError{0};
  std::atomic<bool> inCallback{false};
  static void
  capture(void *p, std::span<const float> samples, const DeviceFormat &format, const CaptureInfo &info) noexcept
  {
    auto &self = *static_cast<Probe *>(p);
    self.inCallback.store(true);
    if (samples.empty() || samples.size() % format.channels != 0 ||
        samples.size() > static_cast<std::size_t>(format.bufferFrames) * format.channels)
      ++self.errors;
    if (info.silent)
      for (float sample : samples)
        if (sample != 0)
          ++self.errors;
    ++self.audioCalls;
    self.inCallback.store(false);
  }
  static void render(void *p, std::span<float> samples, const DeviceFormat &format) noexcept
  {
    auto &self = *static_cast<Probe *>(p);
    self.inCallback.store(true);
    if (samples.empty() || samples.size() % format.channels != 0 ||
        samples.size() > static_cast<std::size_t>(format.bufferFrames) * format.channels)
      ++self.errors;
    for (float sample : samples)
      if (sample != 0)
        ++self.errors;
    ++self.audioCalls; // Leave silence: this test never plays a tone or changes volume.
    self.inCallback.store(false);
  }
  static void status(void *p, const Status &status) noexcept
  {
    auto &self = *static_cast<Probe *>(p);
    if (self.inCallback.load())
      ++self.errors;
    if (status.state == State::Running) {
      if (!status.format.sampleRate || !status.format.channels || !status.format.bufferFrames ||
          status.format.generation <= self.generation.load())
        ++self.errors;
      self.generation.store(status.format.generation);
      ++self.running;
    }
    if (status.state == State::RetryPending)
      ++self.retries;
    if (status.state == State::Stopped)
      ++self.stopped;
    if (status.state == State::Failed)
      ++self.errors;
    if (status.hresult < 0)
      self.lastError.store(status.hresult);
  }
};

template <class F> bool await(F predicate, int milliseconds)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

void lifecycle(Direction direction)
{
  Probe probe;
  const Callbacks callbacks{&probe, Probe::capture, Probe::render, Probe::status};
  WasapiBackend backend(direction, callbacks);
  backend.stop();
  backend.requestRestart();
  CHECK(backend.start());
  CHECK(!backend.start());
  CHECK(await([&] { return probe.running.load() || probe.retries.load() || probe.errors.load(); }, 5000));
  const bool opened = probe.running.load() != 0;
  if (opened) {
    const auto generation = probe.generation.load();
    CHECK(await([&] { return probe.audioCalls.load() != 0; }, 3000));
    if (direction == Direction::LoopbackCapture) {
      // No tone/render source is needed: an idle endpoint must keep producing
      // PCM rather than allowing the receiver's one-second media timeout.
      for (int i = 0; i < 20; ++i) {
        const auto previous = probe.audioCalls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(probe.audioCalls.load() > previous);
      }
    }
    // Keep a real stream active long enough to exercise repeated packet waits.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    backend.requestRestart();
    CHECK(await([&] { return probe.generation.load() > generation; }, 5000));
  } else {
    const auto retries = probe.retries.load();
    CHECK(await([&] { return probe.retries.load() > retries; }, 2500));
    std::printf(
        "%s no-device/retry HRESULT 0x%08lx\n", direction == Direction::Render ? "Render" : "Loopback",
        static_cast<unsigned long>(probe.lastError.load())
    );
  }
  const auto beforeStop = std::chrono::steady_clock::now();
  backend.stop();
  CHECK(std::chrono::steady_clock::now() - beforeStop < std::chrono::seconds(3));
  CHECK(probe.stopped.load() == 1);
  const auto calls = probe.audioCalls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(probe.audioCalls.load() == calls);
  backend.stop();
  const auto generation = probe.generation.load();
  CHECK(backend.start());
  CHECK(await([&] { return probe.generation.load() > generation || probe.retries.load() > 1; }, 5000));
  backend.stop();
  CHECK(probe.stopped.load() == 2);
  CHECK(probe.errors.load() == 0);
  std::printf(
      "%s: opens=%u audio-callbacks=%u retries=%u\n", direction == Direction::Render ? "Render" : "Loopback",
      probe.running.load(), probe.audioCalls.load(), probe.retries.load()
  );
}

void volume()
{
  CHECK(WasapiBackend::setDefaultMasterVolume(-0.1f) == static_cast<std::int32_t>(E_INVALIDARG));
  CHECK(WasapiBackend::setDefaultMasterVolume(1.1f) == static_cast<std::int32_t>(E_INVALIDARG));
  CHECK(
      WasapiBackend::setDefaultMasterVolume(std::numeric_limits<float>::quiet_NaN()) ==
      static_cast<std::int32_t>(E_INVALIDARG)
  );
  // Test in a caller-owned STA: RPC_E_CHANGED_MODE is not a failure for helpers.
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  float value = -123;
  bool muted = true;
  const auto hr = WasapiBackend::readDefaultMasterVolume(value, muted);
  if (hr < 0)
    CHECK(value == -123 && muted);
  else
    CHECK(value >= 0 && value <= 1);
  if (SUCCEEDED(apartment))
    CoUninitialize();
  std::printf("Read default volume HRESULT 0x%08lx (no write performed)\n", static_cast<unsigned long>(hr));
}
} // namespace

int main(int argc, char **argv)
{
  formats();
  conversions();
  if (argc > 1 && std::strcmp(argv[1], "--smoke") == 0) {
    volume();
    lifecycle(Direction::Render);
    lifecycle(Direction::LoopbackCapture);
    // Repeat enough create/stop cycles to catch event/notification handle leaks.
    DWORD before = 0, after = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 12; ++i) {
      WasapiBackend backend(i % 2 ? Direction::Render : Direction::LoopbackCapture, {});
      CHECK(backend.start());
      backend.requestRestart();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      backend.stop();
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    std::printf("Handle count before/after stress: %lu/%lu\n", before, after);
    CHECK(after <= before + 2); // Windows may lazily retain a COM runtime handle.
  }
  std::printf("WASAPI tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
