// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "WasapiBackend.h"
#include "WasapiPcm.h"
#include "DeviceNotificationFilter.h"

#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <initguid.h> // Instantiate mmdeviceapi endpoint-format PROPERTYKEYs.
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace deskflow::audio::windows {
namespace {
using Microsoft::WRL::ComPtr;

class Event final
{
public:
  Event() noexcept = default;
  explicit Event(HANDLE value) noexcept : m_value(value)
  {
  }
  ~Event()
  {
    reset();
  }
  Event(const Event &) = delete;
  Event &operator=(const Event &) = delete;
  HANDLE get() const noexcept
  {
    return m_value;
  }
  bool create(bool manualReset) noexcept
  {
    reset(CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, nullptr));
    return m_value != nullptr;
  }
  void reset(HANDLE value = nullptr) noexcept
  {
    if (m_value)
      CloseHandle(m_value);
    m_value = value;
  }

private:
  HANDLE m_value = nullptr;
};

struct ComApartment final
{
  const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComApartment()
  {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};

class Mmcss final
{
public:
  Mmcss() noexcept
  {
    DWORD index = 0;
    m_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    if (!m_handle)
      m_handle = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    if (!m_handle) {
      m_error = HRESULT_FROM_WIN32(GetLastError());
      if (SUCCEEDED(m_error))
        m_error = E_FAIL;
    } else if (!AvSetMmThreadPriority(m_handle, AVRT_PRIORITY_HIGH)) {
      m_error = HRESULT_FROM_WIN32(GetLastError());
      if (SUCCEEDED(m_error))
        m_error = E_FAIL;
    }
  }
  ~Mmcss()
  {
    if (m_handle)
      AvRevertMmThreadCharacteristics(m_handle);
  }
  HRESULT error() const noexcept
  {
    return m_error;
  }

private:
  HANDLE m_handle = nullptr;
  HRESULT m_error = S_OK;
};

// The notification object owns a DUPLICATE event handle, not an Impl pointer.
// Even a late in-flight COM notification cannot access a destroyed backend or
// signal a closed/recycled HANDLE. No callback takes locks or calls WASAPI.
class DeviceNotifications final : public IMMNotificationClient
{
public:
  explicit DeviceNotifications(HANDLE restart, bool followsDefault) noexcept : m_restart(restart), m_filter(followsDefault)
  {
  }
  void setEndpoint(const wchar_t *id) { m_filter.setEndpoint(id); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
  {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++m_refs;
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG count = --m_refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
  {
    if (m_filter.defaultChanged(flow == eRender, role == eConsole))
      signal();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override
  {
    if (m_filter.endpointChanged(id))
      signal();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
  {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override
  {
    if (m_filter.endpointChanged(id))
      signal();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override
  {
    // In particular do NOT restart on volume changes or friendly-name edits.
    if (m_filter.endpointChanged(id) &&
        (IsEqualPropertyKey(key, PKEY_AudioEngine_DeviceFormat) || IsEqualPropertyKey(key, PKEY_AudioEngine_OEMFormat)))
      signal();
    return S_OK;
  }

private:
  void signal() noexcept
  {
    SetEvent(m_restart.get());
  }
  std::atomic<ULONG> m_refs{1};
  Event m_restart;
  detail::DeviceNotificationFilter m_filter;
};

struct NotificationRegistration final
{
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<DeviceNotifications> callback;
  bool registered = false;
  ~NotificationRegistration()
  {
    if (registered)
      enumerator->UnregisterEndpointNotificationCallback(callback.Get());
    // COM smart pointers release before the worker's ComApartment is destroyed.
  }
  HRESULT initialize(HANDLE restart, bool followsDefault) noexcept
  {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
      return hr;
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(), restart, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS
        ))
      return HRESULT_FROM_WIN32(GetLastError());
    auto *sink = new (std::nothrow) DeviceNotifications(duplicate, followsDefault);
    if (!sink) {
      CloseHandle(duplicate);
      return E_OUTOFMEMORY;
    }
    callback.Attach(sink);
    hr = enumerator->RegisterEndpointNotificationCallback(callback.Get());
    registered = SUCCEEDED(hr);
    return hr;
  }
};

struct Stream final
{
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  ComPtr<IAudioRenderClient> render;
  // Keep the shared audio engine ticking when every application is idle.
  // WASAPI loopback otherwise stops delivering PCM, breaking the media timeline.
  ComPtr<IAudioClient> idleClient;
  ComPtr<IAudioRenderClient> idleRender;
  UINT32 idleBufferFrames = 0;
  Event ready;
  detail::PcmLayout layout;
  DeviceFormat format;
  std::vector<float> scratch;
  DWORD pollMs = 5;
  bool eventDriven = false;
  bool started = false;
  bool idleStarted = false;

  ~Stream()
  {
    stop();
    // WASAPI may still refer to its event until the audio client is released.
    // Close ready only after all services/client/device references are gone.
    capture.Reset();
    render.Reset();
    idleRender.Reset();
    idleClient.Reset();
    client.Reset();
    device.Reset();
  }
  void stop() noexcept
  {
    if (started) {
      client->Stop();
      started = false;
    }
    if (idleStarted) {
      idleClient->Stop();
      idleStarted = false;
    }
  }

  HRESULT activate() noexcept
  {
    capture.Reset();
    render.Reset();
    client.Reset();
    return device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf())
    );
  }

  HRESULT open(IMMDeviceEnumerator *enumerator, DeviceNotifications *notifications, Direction direction, const std::wstring &endpointId)
  {
    HRESULT hr = endpointId.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
                                    : enumerator->GetDevice(endpointId.c_str(), &device);
    if (FAILED(hr))
      return hr;
    LPWSTR id = nullptr;
    if (FAILED(hr = device->GetId(&id)))
      return hr;
    const auto freeId = [](wchar_t *value) { CoTaskMemFree(value); };
    std::unique_ptr<wchar_t, decltype(freeId)> selectedId(id, freeId);
    if (!id)
      return E_FAIL;
    notifications->setEndpoint(id);
    if (FAILED(hr = activate()))
      return hr;

    WAVEFORMATEX *raw = nullptr;
    hr = client->GetMixFormat(&raw);
    const auto freeFormat = [](WAVEFORMATEX *p) { CoTaskMemFree(p); };
    std::unique_ptr<WAVEFORMATEX, decltype(freeFormat)> mix(raw, freeFormat);
    if (FAILED(hr))
      return hr;
    if (!raw || !detail::parseFormat(*raw, layout))
      return AUDCLNT_E_UNSUPPORTED_FORMAT;
    format.sampleRate = raw->nSamplesPerSec;
    format.channels = raw->nChannels;
    format.channelMask = layout.channelMask;

    if (!ready.create(false))
      return HRESULT_FROM_WIN32(GetLastError());
    const DWORD loopback = direction == Direction::LoopbackCapture ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = E_NOINTERFACE;
    if (direction == Direction::Render) {
      ComPtr<IAudioClient3> modern;
      if (SUCCEEDED(client.As(&modern))) {
        UINT32 def = 0, fundamental = 0, minimum = 0, maximum = 0;
        if (SUCCEEDED(modern->GetSharedModeEnginePeriod(raw, &def, &fundamental, &minimum, &maximum))) {
          const UINT32 period = detail::shortestPeriod(fundamental, minimum, maximum);
          if (period)
            hr = modern->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, raw, nullptr);
        }
      }
    }
    // IAudioClient3 documents EVENTCALLBACK, not LOOPBACK, as its supported
    // stream flag. Loopback deliberately uses the supported legacy API.
    if (SUCCEEDED(hr)) {
      eventDriven = true;
      hr = client->SetEventHandle(ready.get());
    }
    if (FAILED(hr)) {
      // An unsuccessful Initialize may leave an audio client unusable. Every
      // fallback activates a new object; it never re-initializes the old one.
      if (FAILED(hr = activate()))
        return hr;
      hr = client->Initialize(
          AUDCLNT_SHAREMODE_SHARED, loopback | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, raw, nullptr
      );
      if (SUCCEEDED(hr))
        hr = client->SetEventHandle(ready.get());
      eventDriven = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
      if (FAILED(hr = activate()))
        return hr;
      REFERENCE_TIME defaultPeriod = 100000, minimumPeriod = 0;
      client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
      // Non-event compatibility path: ask for at least two engine periods.
      const REFERENCE_TIME duration = std::clamp<REFERENCE_TIME>(defaultPeriod, 10000, 1000000) * 2;
      hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback, duration, 0, raw, nullptr);
      eventDriven = false;
    }
    if (FAILED(hr))
      return hr;
    if (FAILED(hr = client->GetBufferSize(&format.bufferFrames)))
      return hr;
    if (!format.bufferFrames || format.bufferFrames > format.sampleRate * 2u)
      return AUDCLNT_E_BUFFER_SIZE_ERROR;

    // All scratch storage is sized afresh for every endpoint/generation, BEFORE
    // Start. Packets or padding that exceed it force teardown, never resize.
    const auto samples = static_cast<std::size_t>(format.bufferFrames) * format.channels;
    scratch.resize(samples);
    const auto halfBufferMs = static_cast<std::uint64_t>(format.bufferFrames) * 500 / format.sampleRate;
    pollMs = static_cast<DWORD>(std::clamp<std::uint64_t>(halfBufferMs, 1, 10));
    if (direction == Direction::Render)
      pollMs = std::min<DWORD>(pollMs, 5);
    if (direction == Direction::LoopbackCapture) {
      if (FAILED(hr = client->GetService(IID_PPV_ARGS(&capture))))
        return hr;
      // Same IMMDevice and mix format as capture; SILENT buffers add no audible
      // content and never change endpoint volume or the user's default device.
      if (FAILED(
              hr = device->Activate(
                  __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(idleClient.GetAddressOf())
              )
          ))
        return hr;
      if (FAILED(hr = idleClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0, raw, nullptr)) ||
          FAILED(hr = idleClient->GetBufferSize(&idleBufferFrames)) ||
          FAILED(hr = idleClient->GetService(IID_PPV_ARGS(&idleRender))))
        return hr;
      return sustainIdleCapture();
    }
    if (FAILED(hr = client->GetService(IID_PPV_ARGS(&render))))
      return hr;
    BYTE *data = nullptr;
    if (FAILED(hr = render->GetBuffer(format.bufferFrames, &data)))
      return hr;
    return render->ReleaseBuffer(format.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
  }

  HRESULT sustainIdleCapture() noexcept
  {
    UINT32 padding = 0;
    HRESULT hr = idleClient->GetCurrentPadding(&padding);
    if (FAILED(hr))
      return hr;
    if (padding > idleBufferFrames)
      return AUDCLNT_E_BUFFER_SIZE_ERROR;
    const auto frames = idleBufferFrames - padding;
    if (!frames)
      return S_OK;
    BYTE *data = nullptr;
    if (FAILED(hr = idleRender->GetBuffer(frames, &data)))
      return hr;
    return idleRender->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
  }

  HRESULT capturePackets(const Callbacks &callbacks, HANDLE stopEvent, HANDLE restartEvent) noexcept
  {
    for (;;) {
      // An endpoint that continuously produces packets must not starve stop or
      // default-device changes. Drain all currently available packets fairly.
      if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 || WaitForSingleObject(restartEvent, 0) == WAIT_OBJECT_0)
        return S_OK;
      UINT32 pending = 0;
      HRESULT hr = capture->GetNextPacketSize(&pending);
      if (FAILED(hr) || !pending)
        return hr;
      if (pending > format.bufferFrames)
        return AUDCLNT_E_BUFFER_SIZE_ERROR;
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 position = 0, qpc = 0;
      hr = capture->GetBuffer(&data, &frames, &flags, &position, &qpc);
      if (hr == AUDCLNT_S_BUFFER_EMPTY)
        return S_OK;
      if (FAILED(hr))
        return hr;
      if (!frames || frames > format.bufferFrames || (!data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))) {
        capture->ReleaseBuffer(frames);
        return AUDCLNT_E_BUFFER_SIZE_ERROR;
      }
      const auto count = static_cast<std::size_t>(frames) * format.channels;
      const std::span<float> samples(scratch.data(), count);
      const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
      detail::decodePcm(silent ? nullptr : data, samples, layout);
      const CaptureInfo info{
          silent, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
          (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0, position, qpc
      };
      if (callbacks.capture)
        callbacks.capture(callbacks.context, samples, format, info);
      hr = capture->ReleaseBuffer(frames);
      if (FAILED(hr))
        return hr;
    }
  }

  HRESULT renderAvailable(const Callbacks &callbacks) noexcept
  {
    UINT32 padding = 0;
    HRESULT hr = client->GetCurrentPadding(&padding);
    if (FAILED(hr))
      return hr;
    if (padding > format.bufferFrames)
      return AUDCLNT_E_BUFFER_SIZE_ERROR;
    // Do not hand a delayed wakeup's entire free buffer to the receiver. One
    // callback per wake preserves its bounded quantum; timeouts refill any
    // remaining space even when the driver does not issue another event.
    const UINT32 frames = detail::renderQuantum(format.bufferFrames - padding, format.sampleRate);
    if (!frames)
      return S_OK;
    const auto count = static_cast<std::size_t>(frames) * format.channels;
    const std::span<float> samples(scratch.data(), count);
    std::fill(samples.begin(), samples.end(), 0.0f);
    if (callbacks.render)
      callbacks.render(callbacks.context, samples, format);
    BYTE *data = nullptr;
    if (FAILED(hr = render->GetBuffer(frames, &data)))
      return hr;
    if (!data) {
      render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
      return E_POINTER;
    }
    detail::encodePcm(samples, data, layout);
    return render->ReleaseBuffer(frames, callbacks.render ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
  }

  // The only active real-time loop. No vector growth, locks, status callbacks,
  // logging, COM activation, notification registration, or endpoint queries.
  HRESULT run(const Callbacks &callbacks, HANDLE stopEvent, HANDLE restartEvent) noexcept
  {
    HRESULT hr = S_OK;
    if (idleClient) {
      hr = idleClient->Start();
      if (FAILED(hr))
        return hr;
      idleStarted = true;
    }
    hr = client->Start();
    if (FAILED(hr))
      return hr;
    started = true;
    const HANDLE events[] = {stopEvent, restartEvent, ready.get()};
    for (;;) {
      // Timeouts also drain loopback on pre-1703 systems whose event never fires.
      // They allow silent endpoints and drivers with lost events to stay usable.
      const DWORD result = WaitForMultipleObjects(eventDriven ? 3 : 2, events, FALSE, pollMs);
      if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1)
        return S_OK;
      if (result == WAIT_FAILED)
        return HRESULT_FROM_WIN32(GetLastError());
      if (result != WAIT_TIMEOUT && result != WAIT_OBJECT_0 + 2)
        return E_UNEXPECTED;
      if (capture && FAILED(hr = sustainIdleCapture()))
        return hr;
      hr = capture ? capturePackets(callbacks, stopEvent, restartEvent) : renderAvailable(callbacks);
      if (FAILED(hr))
        return hr;
    }
  }
};

HRESULT defaultVolume(ComPtr<IAudioEndpointVolume> &volume) noexcept
{
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr))
    return hr;
  ComPtr<IMMDevice> device;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (FAILED(hr))
    return hr;
  return device->Activate(
      __uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(volume.GetAddressOf())
  );
}

} // namespace

struct WasapiBackend::Impl final
{
  Direction direction;
  Callbacks callbacks;
  std::wstring endpointId;
  Event stopEvent;
  Event restartEvent;
  std::thread worker;
  std::uint64_t generation = 0; // Worker-owned; retained across stop/start.

  Impl(Direction value, Callbacks cb, std::wstring id) : direction(value), callbacks(cb), endpointId(std::move(id))
  {
  }

  void status(State state, HRESULT hr = S_OK, DeviceFormat format = {}) const noexcept
  {
    if (callbacks.status)
      callbacks.status(callbacks.context, Status{state, static_cast<std::int32_t>(hr), format});
  }

  bool stopping() const noexcept
  {
    return WaitForSingleObject(stopEvent.get(), 0) == WAIT_OBJECT_0;
  }

  void execute()
  {
    status(State::Starting);
    ComApartment apartment;
    if (FAILED(apartment.result)) {
      status(State::Failed, apartment.result);
      return;
    }
    NotificationRegistration notifications;
    HRESULT hr = notifications.initialize(restartEvent.get(), endpointId.empty());
    if (FAILED(hr)) {
      // Without notifications default tracking is not guaranteed; do not
      // silently leave a stream attached to a stale default endpoint.
      status(State::Failed, hr);
      return;
    }
    while (!stopping()) {
      ResetEvent(restartEvent.get()); // Changes during open remain signalled.
      DeviceFormat lastFormat{};
      {
        Stream stream;
        hr = stream.open(notifications.enumerator.Get(), notifications.callback.Get(), direction, endpointId);
        if (SUCCEEDED(hr) && !stopping() && WaitForSingleObject(restartEvent.get(), 0) != WAIT_OBJECT_0) {
          stream.format.generation = ++generation;
          lastFormat = stream.format;
          {
            Mmcss scheduling;
            if (FAILED(scheduling.error()))
              status(State::SchedulingWarning, scheduling.error(), lastFormat);
            // Running means the stream is prepared and is about to Start. This
            // callback MUST finish before Start/the first active audio loop.
            status(State::Running, S_OK, lastFormat);
            if (!stopping() && WaitForSingleObject(restartEvent.get(), 0) != WAIT_OBJECT_0)
              hr = stream.run(callbacks, stopEvent.get(), restartEvent.get());
            stream.stop(); // Stop before reverting MMCSS or emitting any status.
          }
        }
      } // All WASAPI interfaces and storage freed outside the active loop.
      if (stopping())
        break;
      status(State::Restarting, hr, lastFormat);
      if (FAILED(hr)) {
        status(State::RetryPending, hr, lastFormat);
        const HANDLE events[] = {stopEvent.get(), restartEvent.get()};
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, 1000);
        if (result == WAIT_OBJECT_0)
          break;
        if (result == WAIT_FAILED) {
          status(State::Failed, HRESULT_FROM_WIN32(GetLastError()), lastFormat);
          return;
        }
      }
    }
    status(State::Stopped);
  }

  void entry() noexcept
  {
    try {
      execute();
    } catch (const std::bad_alloc &) {
      status(State::Failed, E_OUTOFMEMORY);
    } catch (...) {
      status(State::Failed, E_FAIL);
    }
  }
};

WasapiBackend::WasapiBackend(Direction direction, Callbacks callbacks, std::wstring endpointId)
    : m_impl(std::make_unique<Impl>(direction, callbacks, std::move(endpointId)))
{
}
WasapiBackend::~WasapiBackend()
{
  stop();
}

bool WasapiBackend::start() noexcept
{
  if (m_impl->worker.joinable())
    return false;
  if (!m_impl->stopEvent.create(true))
    return false;
  if (!m_impl->restartEvent.create(true)) {
    m_impl->stopEvent.reset();
    return false;
  }
  try {
    m_impl->worker = std::thread([impl = m_impl.get()] { impl->entry(); });
    return true;
  } catch (...) {
    m_impl->restartEvent.reset();
    m_impl->stopEvent.reset();
    return false;
  }
}

void WasapiBackend::stop() noexcept
{
  if (!m_impl->worker.joinable())
    return;
  SetEvent(m_impl->stopEvent.get());
  m_impl->worker.join(); // Cooperative; never TerminateThread or detach.
  // Keep events alive until next start/destruction. Lifecycle operations must
  // be externally serialized, including requestRestart versus stop/start.
}

void WasapiBackend::requestRestart() noexcept
{
  if (m_impl->restartEvent.get())
    SetEvent(m_impl->restartEvent.get());
}

std::int32_t WasapiBackend::defaultRenderEndpointId(std::wstring &id) noexcept
{
  ComApartment apartment;
  if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
    return apartment.result;
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr))
    return hr;
  ComPtr<IMMDevice> device;
  if (FAILED(hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
    return hr;
  LPWSTR value = nullptr;
  hr = device->GetId(&value);
  if (SUCCEEDED(hr)) {
    try {
      id.assign(value);
    } catch (...) {
      hr = E_OUTOFMEMORY;
    }
  }
  CoTaskMemFree(value);
  return hr;
}

std::int32_t WasapiBackend::readDefaultMasterVolume(float &scalar, bool &muted) noexcept
{
  ComApartment apartment;
  // Helpers also work in a caller-owned STA; do not uninitialize that apartment.
  if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
    return apartment.result;
  ComPtr<IAudioEndpointVolume> volume;
  HRESULT hr = defaultVolume(volume);
  if (FAILED(hr))
    return hr;
  float value = 0;
  BOOL mute = FALSE;
  hr = volume->GetMasterVolumeLevelScalar(&value);
  if (SUCCEEDED(hr))
    hr = volume->GetMute(&mute);
  if (SUCCEEDED(hr)) {
    scalar = value;
    muted = mute != FALSE;
  }
  return hr;
}

std::int32_t WasapiBackend::setDefaultMasterVolume(float scalar) noexcept
{
  if (!std::isfinite(scalar) || scalar < 0.0f || scalar > 1.0f)
    return E_INVALIDARG;
  ComApartment apartment;
  if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
    return apartment.result;
  ComPtr<IAudioEndpointVolume> volume;
  const HRESULT hr = defaultVolume(volume);
  if (FAILED(hr))
    return hr;
  return volume->SetMasterVolumeLevelScalar(scalar, nullptr);
}

} // namespace deskflow::audio::windows
