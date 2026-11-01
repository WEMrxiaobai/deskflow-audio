// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Explicit, standalone Windows hardware test; intentionally not in CMake/CTest.
// Build this translation unit together with windows/WasapiBackend.cpp using
// C++20 and link ole32.lib, uuid.lib and avrt.lib. Run ONLY by explicit request:
//   EndpointMuteCaptureTests.exe --manual-endpoint-mute
// Exit: 0 = supported + state restored, 1 = failed/unsupported, 77 = inconclusive.
// No PCM/content is retained or written: only aggregate sample counts and peaks.
// No network, microphone, capture->render path, or volume-scalar setter is used.
//
// Theory is NOT a portability guarantee: WASAPI can use a hardware loopback pin
// or copy the audio-engine output; endpoint mute can be hardware or software.
// A downstream mute would preserve upstream loopback, but the tap/control order
// must be measured on the actual endpoint/driver, not assumed from that model.
// https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording
// https://learn.microsoft.com/en-us/windows/win32/coreaudio/endpoint-volume-controls

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

#include "../windows/WasapiBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace deskflow::audio::windows;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
constexpr int kInconclusive = 77;
constexpr float kSignalPeak = 0.00005f;
constexpr float kScalarTolerance = 0.000001f;
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);

void require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void requireHr(HRESULT hr, const char *operation)
{
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s: HRESULT 0x%08lx\n", operation, static_cast<unsigned long>(hr));
    throw std::runtime_error(operation);
  }
}

struct ComApartment final
{
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComApartment()
  {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};

struct TaskMemoryFree
{
  void operator()(wchar_t *p) const noexcept
  {
    CoTaskMemFree(p);
  }
};

std::wstring deviceId(IMMDevice *device)
{
  wchar_t *raw = nullptr;
  const HRESULT hr = device->GetId(&raw);
  const std::unique_ptr<wchar_t, TaskMemoryFree> owned(raw);
  requireHr(hr, "IMMDevice::GetId");
  require(raw != nullptr, "Endpoint returned no ID");
  return std::wstring(raw);
}

struct Endpoint final
{
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioEndpointVolume> volume;
  std::wstring id;

  Endpoint()
  {
    requireHr(
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
        "Create endpoint enumerator"
    );
    requireHr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Get default render/console endpoint");
    id = deviceId(device.Get());
    requireHr(
        device->Activate(
            __uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(volume.GetAddressOf())
        ),
        "Activate endpoint volume"
    );
  }

  void requireStillDefault() const
  {
    ComPtr<IMMDevice> current;
    requireHr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current), "Recheck default endpoint");
    require(deviceId(current.Get()) == id, "Default endpoint changed: result invalid (original endpoint will be restored)");
  }
};

struct RestoreReport
{
  bool checked = false;
  bool muteRestored = false;
  bool scalarUnchanged = false;
};

// Lives OUTSIDE exercise(), so both backend destructors have stopped/joined the
// tone and capture workers before an original unmuted state is restored. Armed
// BEFORE SetMute: even a failed/throwing subsequent operation takes this path.
// Normal early returns and all C++ exceptions restore; process kill/power loss
// cannot be made safe by an in-process RAII guard.
class RestoreEndpoint final
{
public:
  RestoreEndpoint(IAudioEndpointVolume *volume, BOOL mute, float scalar, RestoreReport &report) noexcept
      : m_volume(volume), m_mute(mute), m_scalar(scalar), m_report(report)
  {
  }
  ~RestoreEndpoint() noexcept
  {
    HRESULT setResult = S_OK;
    if (m_armed) {
      setResult = m_volume->SetMute(m_mute, nullptr);
      if (FAILED(setResult)) // Best-effort retry, but never claim success without read-back.
        setResult = m_volume->SetMute(m_mute, nullptr);
    }
    BOOL mute = !m_mute;
    float scalar = -1.0f;
    const HRESULT muteResult = m_volume->GetMute(&mute);
    const HRESULT scalarResult = m_volume->GetMasterVolumeLevelScalar(&scalar);
    m_report.checked = true;
    m_report.muteRestored = SUCCEEDED(setResult) && SUCCEEDED(muteResult) && !!mute == !!m_mute;
    m_report.scalarUnchanged =
        SUCCEEDED(scalarResult) && std::isfinite(scalar) && std::fabs(scalar - m_scalar) <= kScalarTolerance;
    std::printf(
        "RESTORE original_mute=%d actual_mute=%d mute_restored=%s original_scalar=%.9g actual_scalar=%.9g "
        "scalar_unchanged=%s\n",
        !!m_mute, !!mute, m_report.muteRestored ? "true" : "false", static_cast<double>(m_scalar),
        static_cast<double>(scalar), m_report.scalarUnchanged ? "true" : "false"
    );
    if (!m_report.muteRestored || !m_report.scalarUnchanged)
      std::fprintf(
          stderr,
          "RESTORE FAILED / external volume change: SetMute=0x%08lx GetMute=0x%08lx GetScalar=0x%08lx; "
          "inspect Windows sound controls. Scalar is deliberately NEVER written.\n",
          static_cast<unsigned long>(setResult), static_cast<unsigned long>(muteResult),
          static_cast<unsigned long>(scalarResult)
      );
    std::fflush(stdout);
  }
  HRESULT mute() noexcept
  {
    m_armed = true;
    return m_volume->SetMute(TRUE, nullptr);
  }

private:
  IAudioEndpointVolume *m_volume;
  BOOL m_mute;
  float m_scalar;
  RestoreReport &m_report;
  bool m_armed = false;
};

struct Probe final
{
  std::atomic<unsigned> requestedWindow{0}, activeWindow{0};
  std::atomic<std::uint64_t> count{0}, callbacks{0};
  std::atomic<float> peak{0};
  std::atomic<unsigned> running{0}, interruptions{0}, sampleRate{0}, channels{0};
  std::atomic<std::uint64_t> generation{0};
  std::atomic<std::int32_t> lastError{0};
  // These fields each have exactly one worker owner, never the main thread.
  unsigned captureWindow = 0;
  std::uint64_t windowCount = 0, windowCallbacks = 0;
  float windowPeak = 0;
  double phase = 0;

  static void capture(
      void *context, std::span<const float> samples, const DeviceFormat &, const CaptureInfo &
  ) noexcept
  {
    auto &self = *static_cast<Probe *>(context);
    const unsigned window = self.requestedWindow.load(std::memory_order_acquire);
    if (window != self.captureWindow) {
      self.captureWindow = window;
      self.windowCount = self.windowCallbacks = 0;
      self.windowPeak = 0;
    }
    for (float sample : samples)
      if (std::isfinite(sample))
        self.windowPeak = (std::max)(self.windowPeak, std::fabs(sample));
    self.windowCount += static_cast<std::uint64_t>(samples.size());
    ++self.windowCallbacks;
    self.peak.store(self.windowPeak, std::memory_order_relaxed);
    self.count.store(self.windowCount, std::memory_order_relaxed);
    self.callbacks.store(self.windowCallbacks, std::memory_order_relaxed);
    // Acknowledge only after the callback-owned reset + first complete packet.
    // No pre-mute peak can leak through a main-thread/reset race.
    self.activeWindow.store(window, std::memory_order_release);
  }

  static void render(void *context, std::span<float> samples, const DeviceFormat &format) noexcept
  {
    auto &self = *static_cast<Probe *>(context);
    if (!format.sampleRate || !format.channels)
      return; // Destination was zero-filled by WasapiBackend.
    constexpr double twoPi = 6.283185307179586476925286766559;
    const double increment = twoPi * 440.0 / static_cast<double>(format.sampleRate);
    for (std::size_t frame = 0; frame + format.channels <= samples.size(); frame += format.channels) {
      const float value = 0.01f * static_cast<float>(std::sin(self.phase));
      for (std::size_t channel = 0; channel < format.channels; ++channel)
        samples[frame + channel] = value;
      self.phase += increment;
      if (self.phase >= twoPi)
        self.phase -= twoPi;
    }
  }

  static void status(void *context, const Status &status) noexcept
  {
    auto &self = *static_cast<Probe *>(context);
    if (status.state == State::Running) {
      self.sampleRate.store(status.format.sampleRate);
      self.channels.store(status.format.channels);
      self.generation.store(status.format.generation);
      self.running.fetch_add(1);
    }
    if (status.state == State::Restarting || status.state == State::RetryPending || status.state == State::Failed)
      self.interruptions.fetch_add(1);
    if (status.hresult < 0)
      self.lastError.store(status.hresult);
  }
};

template <class Predicate> bool await(Predicate predicate, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (predicate())
      return true;
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

struct Measurement
{
  float peak = 0;
  std::uint64_t count = 0;
  std::uint64_t callbacks = 0;
  bool windowAcknowledged = false;
};

Measurement measure(Probe &capture)
{
  const unsigned window = capture.requestedWindow.fetch_add(1, std::memory_order_release) + 1;
  const bool acknowledged = await([&] { return capture.activeWindow.load(std::memory_order_acquire) == window; }, 1500ms);
  if (!acknowledged)
    return {}; // No packets is an observed absence, not a stale peak from the last phase.
  std::this_thread::sleep_for(1000ms);
  return {
      capture.peak.load(), capture.count.load(), capture.callbacks.load(), true
  };
}

void printMeasurement(const char *label, const Measurement &measurement)
{
  std::printf(
      "%s: peak=%.9g samples=%llu callbacks=%llu reset_acknowledged=%s\n", label,
      static_cast<double>(measurement.peak), static_cast<unsigned long long>(measurement.count),
      static_cast<unsigned long long>(measurement.callbacks), measurement.windowAcknowledged ? "true" : "false"
  );
}

bool hasSignal(const Measurement &measurement, std::uint64_t minimumSamples, float minimumPeak)
{
  return measurement.windowAcknowledged && measurement.count >= minimumSamples && measurement.callbacks >= 2 &&
         std::isfinite(measurement.peak) && measurement.peak >= minimumPeak;
}

enum class Support { Unknown, Supported, Unsupported };
const char *supportName(Support support)
{
  switch (support) {
  case Support::Supported:
    return "true";
  case Support::Unsupported:
    return "false";
  default:
    return "unknown";
  }
}

void verifyState(Endpoint &endpoint, BOOL expectedMute, float originalScalar)
{
  endpoint.requireStillDefault();
  BOOL mute = FALSE;
  float scalar = -1;
  requireHr(endpoint.volume->GetMute(&mute), "Read mute state");
  requireHr(endpoint.volume->GetMasterVolumeLevelScalar(&scalar), "Read volume scalar");
  require(!!mute == !!expectedMute, "Endpoint mute did not match expected state / changed externally");
  require(std::isfinite(scalar) && std::fabs(scalar - originalScalar) <= kScalarTolerance, "Volume scalar changed externally");
}

int exercise(Endpoint &endpoint, RestoreEndpoint &restore, BOOL originalMute, float originalScalar, Support &support)
{
  if (originalMute) {
    std::puts("SKIP: endpoint was already muted; will NOT automatically unmute it to obtain a baseline.");
    return kInconclusive;
  }
  Probe captureProbe, renderProbe;
  const Callbacks captureCallbacks{&captureProbe, Probe::capture, nullptr, Probe::status};
  const Callbacks renderCallbacks{&renderProbe, nullptr, Probe::render, Probe::status};
  WasapiBackend capture(Direction::LoopbackCapture, captureCallbacks);
  WasapiBackend render(Direction::Render, renderCallbacks);
  require(capture.start(), "Cannot start loopback worker");
  require(render.start(), "Cannot start synthetic render worker");
  require(
      await([&] { return captureProbe.running.load() && renderProbe.running.load(); }, 5000ms),
      "Streams did not reach Running (device unavailable or unsupported)"
  );
  const auto captureGeneration = captureProbe.generation.load();
  const auto renderGeneration = renderProbe.generation.load();
  const auto requireStableStreams = [&] {
    require(
        captureProbe.generation.load() == captureGeneration && renderProbe.generation.load() == renderGeneration &&
            captureProbe.interruptions.load() == 0 && renderProbe.interruptions.load() == 0,
        "Stream restart/device loss makes the mute comparison inconclusive"
    );
  };
  const std::uint64_t minimumSamples =
      static_cast<std::uint64_t>(captureProbe.sampleRate.load()) * captureProbe.channels.load() / 2;
  require(minimumSamples != 0, "Invalid capture format");
  verifyState(endpoint, FALSE, originalScalar);
  std::this_thread::sleep_for(250ms);
  const Measurement baseline = measure(captureProbe);
  printMeasurement("BASELINE unmuted", baseline);
  requireStableStreams();
  verifyState(endpoint, FALSE, originalScalar);
  if (!hasSignal(baseline, minimumSamples, kSignalPeak)) {
    std::puts("INCONCLUSIVE: no adequate unmuted baseline signal; no mute was applied, scalar will not be raised.");
    return kInconclusive;
  }

  std::puts("Applying temporary endpoint SetMute(TRUE); discarding the next 1 second before a fresh capture window.");
  std::fflush(stdout);
  requireHr(restore.mute(), "Apply temporary endpoint mute");
  verifyState(endpoint, TRUE, originalScalar);
  std::this_thread::sleep_for(1000ms);
  const Measurement muted = measure(captureProbe);
  printMeasurement("MUTED after 1s settling", muted);
  verifyState(endpoint, TRUE, originalScalar);
  requireStableStreams();
  const float minimumMutedPeak = (std::max)(kSignalPeak, baseline.peak * 0.05f);
  const bool preserved = hasSignal(muted, minimumSamples, minimumMutedPeak);
  std::printf(
      "CRITERIA minimum_samples=%llu minimum_peak=%.9g muted_to_baseline_peak=%.6g\n",
      static_cast<unsigned long long>(minimumSamples), static_cast<double>(minimumMutedPeak),
      static_cast<double>(muted.peak / baseline.peak)
  );
  // Stop/join BEFORE returning into the RAII restore (no tone after unmute).
  render.stop();
  capture.stop();
  requireStableStreams();
  support = preserved ? Support::Supported : Support::Unsupported;
  std::puts(
      preserved ? "OBSERVED: capture retains signal while this endpoint reports muted."
                : "OBSERVED: muted capture did not meet the signal/count thresholds; do not rely on mute-preserving loopback."
  );
  std::puts("No listening/microphone check was made: endpoint mute read-back is not acoustic proof of speaker silence.");
  return preserved ? 0 : 1;
}
} // namespace

int main(int argc, char **argv)
{
  if (argc != 2 || std::strcmp(argv[1], "--manual-endpoint-mute") != 0) {
    std::puts(
        "NOT RUN: explicit manual Windows hardware test.\n"
        "Usage: EndpointMuteCaptureTests.exe --manual-endpoint-mute\n"
        "Temporarily affects the default speakers; never run automatically in CI/CTest."
    );
    return kInconclusive;
  }
  std::puts(
      "MANUAL WINDOWS HARDWARE TEST START: default eRender/eConsole speakers.\n"
      "WARNING: plays a quiet 440 Hz / 0.01-peak synthetic tone and temporarily mutes ALL audio on that endpoint.\n"
      "The original mute state is restored by RAII on normal returns and C++ exceptions; master scalar is NEVER written.\n"
      "An already-muted endpoint is NOT unmuted (test skips). Do not change device/volume during this test.\n"
      "Close other audio sources for an attributable baseline. Only aggregate peak/count are kept; NO PCM/content files.\n"
      "Render is synthetic ONLY: no microphone, capture->render feedback, or network. Allow it to finish; do not kill it."
  );
  std::fflush(stdout);
  Support support = Support::Unknown;
  RestoreReport report;
  int result = 1;
  try {
    ComApartment apartment;
    requireHr(apartment.result, "Initialize COM");
    Endpoint endpoint;
    BOOL originalMute = FALSE;
    float originalScalar = -1;
    requireHr(endpoint.volume->GetMute(&originalMute), "Snapshot original mute");
    requireHr(endpoint.volume->GetMasterVolumeLevelScalar(&originalScalar), "Snapshot original master scalar");
    require(std::isfinite(originalScalar) && originalScalar >= 0 && originalScalar <= 1, "Invalid original scalar");
    std::printf("ORIGINAL mute=%d scalar=%.9g (read-only)\n", !!originalMute, static_cast<double>(originalScalar));
    {
      RestoreEndpoint restore(endpoint.volume.Get(), originalMute, originalScalar, report);
      try {
        result = exercise(endpoint, restore, originalMute, originalScalar, support);
      } catch (const std::exception &error) {
        std::fprintf(stderr, "TEST ERROR: %s\n", error.what());
        result = 1;
      } catch (...) {
        std::fputs("TEST ERROR: unknown exception\n", stderr);
        result = 1;
      }
    } // finally: workers have unwound, restore original mute, verify scalar.
    if (!report.checked || !report.muteRestored || !report.scalarUnchanged)
      result = 1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "SETUP ERROR (no mute applied): %s\n", error.what());
    result = 1;
  } catch (...) {
    std::fputs("SETUP ERROR: unknown exception\n", stderr);
    result = 1;
  }
  std::printf(
      "RESULT loopback_muted_supported=%s mute_restored=%s scalar_unchanged=%s exit_code=%d\n", supportName(support),
      report.checked ? (report.muteRestored ? "true" : "false") : "not_needed",
      report.checked ? (report.scalarUnchanged ? "true" : "false") : "not_checked", result
  );
  return result;
}
