// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace deskflow::audio::windows {

struct DeviceFormat
{
  std::uint32_t sampleRate = 0;
  std::uint16_t channels = 0;
  std::uint32_t channelMask = 0; // Windows SPEAKER_* bits; zero means unspecified.
  std::uint32_t bufferFrames = 0;
  std::uint64_t generation = 0; // Changes on every successful stream recreation.
};

struct CaptureInfo
{
  bool silent = false;
  bool discontinuity = false;
  bool timestampValid = false;
  std::uint64_t devicePosition = 0;
  std::uint64_t qpcPosition100ns = 0;
};

enum class Direction
{
  LoopbackCapture,
  Render
};
enum class State
{
  Starting,
  Running,
  Restarting,
  RetryPending,
  Stopped,
  Failed,
  SchedulingWarning
};

struct Status
{
  State state;
  std::int32_t hresult = 0;
  DeviceFormat format{};
};

struct Callbacks
{
  void *context = nullptr;
  // Borrowed interleaved samples, valid only for this call. Copy to a bounded
  // queue; NEVER encode, network, log, wait, allocate, or control Backend here.
  void (*capture)(void *, std::span<const float>, const DeviceFormat &, const CaptureInfo &) noexcept = nullptr;
  // Destination is zero-filled. Mix available sources at this exact format;
  // leave missing frames zero. Same real-time restrictions as capture.
  void (*render)(void *, std::span<float>, const DeviceFormat &) noexcept = nullptr;
  // Called on the backend worker ONLY outside the active MMCSS audio loop.
  // May post to a UI queue, but must return promptly; never stop/destroy this
  // backend here. No synchronous status logging on the audio path.
  void (*status)(void *, const Status &) noexcept = nullptr;
};

// One independent stream/worker per instance. Uses default eRender/eConsole,
// never a microphone, and never changes the Windows default device or volume.
// Lifecycle calls must be externally serialized and made outside callbacks.
class WasapiBackend final
{
public:
  explicit WasapiBackend(Direction direction, Callbacks callbacks, std::wstring endpointId = {});
  ~WasapiBackend();
  WasapiBackend(const WasapiBackend &) = delete;
  WasapiBackend &operator=(const WasapiBackend &) = delete;

  // Asynchronous device open. False means thread/event startup failed or this
  // instance is already started. Device-open errors arrive through status.
  bool start() noexcept;
  void stop() noexcept;           // Cooperative signal + join; callback must not block.
  void requestRestart() noexcept; // Safe to signal from a power-event handler.

  // Non-real-time helpers. HRESULT returned as signed 32-bit; output unchanged
  // on failure. Scalar [0,1], not dB. Each call resolves the current default.
  static std::int32_t defaultRenderEndpointId(std::wstring &id) noexcept;
  static std::int32_t readDefaultMasterVolume(float &scalar, bool &muted) noexcept;
  static std::int32_t setDefaultMasterVolume(float scalar) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::audio::windows
