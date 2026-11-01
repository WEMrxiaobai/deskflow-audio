/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "EndpointMuteGuard.h"

#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <new>
#include <utility>

namespace deskflow::audio::windows {
namespace {
using Microsoft::WRL::ComPtr;
static_assert(std::atomic<bool>::is_always_lock_free);

class MuteNotification final : public IAudioEndpointVolumeCallback
{
public:
  explicit MuteNotification(const GUID &context) noexcept : ownContext(context) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
  {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IAudioEndpointVolumeCallback))
      return E_NOINTERFACE;
    *out = static_cast<IAudioEndpointVolumeCallback *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const auto count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notification) override
  {
    if (!notification)
      return E_POINTER;
    if (!IsEqualGUID(notification->guidEventContext, ownContext))
      lost.store(true, std::memory_order_release);
    return S_OK;
  }
  const GUID ownContext;
  std::atomic<bool> lost{false};
private:
  std::atomic<ULONG> refs{1};
};

class NativeEndpoint final : public EndpointMuteGuard::Endpoint
{
public:
  explicit NativeEndpoint(ComPtr<IAudioEndpointVolume> volume) noexcept : m_volume(std::move(volume)) {}
  HRESULT getMute(BOOL &muted) noexcept override { return m_volume->GetMute(&muted); }
  HRESULT setMute(BOOL muted, const GUID &context) noexcept override { return m_volume->SetMute(muted, &context); }
  HRESULT subscribe(IAudioEndpointVolumeCallback *callback) noexcept override
  {
    return m_volume->RegisterControlChangeNotify(callback);
  }
  HRESULT unsubscribe(IAudioEndpointVolumeCallback *callback) noexcept override
  {
    return m_volume->UnregisterControlChangeNotify(callback);
  }
private:
  ComPtr<IAudioEndpointVolume> m_volume;
};
} // namespace

struct EndpointMuteGuard::Impl
{
  std::unique_ptr<Endpoint> endpoint;
  ComPtr<MuteNotification> notification;
  std::wstring id;
  DWORD threadId = GetCurrentThreadId();
  bool original = false;
  bool acquired = false;
  bool initialized = false;
  bool subscribed = false;
  ~Impl()
  {
    if (subscribed)
      (void)endpoint->unsubscribe(notification.Get());
    // The notification object never refers to Impl. A COM-retained/in-flight
    // callback remains safe even if unregistration fails during device loss.
  }
  bool lost() const noexcept
  {
    return notification && notification->lost.load(std::memory_order_acquire);
  }
};

EndpointMuteGuard::EndpointMuteGuard() noexcept : m_impl(new (std::nothrow) Impl) {}
EndpointMuteGuard::~EndpointMuteGuard() { (void)restore(); }

HRESULT EndpointMuteGuard::initialize(const wchar_t *exactEndpointId) noexcept
{
  if (!m_impl)
    return E_OUTOFMEMORY;
  if (GetCurrentThreadId() != m_impl->threadId)
    return RPC_E_WRONG_THREAD;
  if (m_impl->initialized || m_impl->endpoint)
    return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
  try {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
      return hr;
    ComPtr<IMMDevice> device;
    hr = exactEndpointId && *exactEndpointId ? enumerator->GetDevice(exactEndpointId, &device)
                                          : enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr))
      return hr;
    wchar_t *rawId = nullptr;
    hr = device->GetId(&rawId);
    if (FAILED(hr))
      return hr;
    std::wstring id;
    try {
      id = rawId;
    } catch (...) {
      CoTaskMemFree(rawId);
      return E_OUTOFMEMORY;
    }
    CoTaskMemFree(rawId);
    ComPtr<IAudioEndpointVolume> volume;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
    if (FAILED(hr))
      return hr;
    return initialize(std::make_unique<NativeEndpoint>(std::move(volume)), std::move(id));
  } catch (...) {
    return E_OUTOFMEMORY;
  }
}

HRESULT EndpointMuteGuard::initialize(std::unique_ptr<Endpoint> endpoint, std::wstring exactEndpointId) noexcept
{
  if (!m_impl)
    return E_OUTOFMEMORY;
  auto &s = *m_impl;
  if (GetCurrentThreadId() != s.threadId)
    return RPC_E_WRONG_THREAD;
  if (s.initialized || s.endpoint)
    return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
  if (!endpoint || exactEndpointId.empty())
    return E_INVALIDARG;
  GUID context{};
  HRESULT hr = CoCreateGuid(&context);
  if (FAILED(hr))
    return hr;
  ComPtr<MuteNotification> notification;
  notification.Attach(new (std::nothrow) MuteNotification(context));
  if (!notification)
    return E_OUTOFMEMORY;
  // Register BEFORE snapshotting; an external event during GetMute invalidates
  // ownership instead of being erased by a late reset of the atomic lost flag.
  hr = endpoint->subscribe(notification.Get());
  if (FAILED(hr))
    return hr;
  BOOL original = FALSE;
  hr = endpoint->getMute(original);
  if (FAILED(hr)) {
    (void)endpoint->unsubscribe(notification.Get());
    return hr;
  }
  s.endpoint = std::move(endpoint);
  s.notification = std::move(notification);
  s.id = std::move(exactEndpointId);
  s.original = original != FALSE;
  s.subscribed = true;
  s.initialized = true;
  return S_OK;
}

const std::wstring &EndpointMuteGuard::endpointId() const noexcept
{
  static const std::wstring empty;
  return m_impl ? m_impl->id : empty;
}
bool EndpointMuteGuard::ownershipLost() const noexcept { return m_impl && m_impl->lost(); }
bool EndpointMuteGuard::mutedByUs() const noexcept { return m_impl && m_impl->acquired && !m_impl->lost(); }

HRESULT EndpointMuteGuard::update(bool localPlayback) noexcept
{
  if (localPlayback)
    return restore();
  if (!m_impl)
    return E_OUTOFMEMORY;
  auto &s = *m_impl;
  if (GetCurrentThreadId() != s.threadId)
    return RPC_E_WRONG_THREAD;
  if (!s.initialized)
    return CO_E_NOTINITIALIZED;
  if (s.lost())
    return S_FALSE;
  if (s.original || s.acquired)
    return S_OK;
  BOOL current = FALSE;
  HRESULT hr = s.endpoint->getMute(current);
  if (FAILED(hr))
    return hr;
  if (s.lost())
    return S_FALSE;
  if (current) {
    // An external mute may precede its asynchronous notification. Never claim
    // an already-muted endpoint, even before the callback has caught up.
    s.notification->lost.store(true, std::memory_order_release);
    return S_FALSE;
  }
  hr = s.endpoint->setMute(TRUE, s.notification->ownContext);
  if (SUCCEEDED(hr))
    s.acquired = true;
  return hr;
}

HRESULT EndpointMuteGuard::restore() noexcept
{
  if (!m_impl)
    return E_OUTOFMEMORY;
  auto &s = *m_impl;
  if (GetCurrentThreadId() != s.threadId)
    return RPC_E_WRONG_THREAD;
  if (!s.initialized)
    return S_OK;
  if (s.lost()) {
    s.acquired = false;
    return S_FALSE;
  }
  if (!s.acquired || s.original)
    return S_OK;
  BOOL current = FALSE;
  HRESULT hr = s.endpoint->getMute(current);
  if (FAILED(hr))
    return hr; // retain ownership so a subsequent explicit restore can retry
  if (s.lost()) {
    s.acquired = false;
    return S_FALSE;
  }
  if (!current) {
    s.notification->lost.store(true, std::memory_order_release);
    s.acquired = false;
    return S_FALSE;
  }
  hr = s.endpoint->setMute(FALSE, s.notification->ownContext);
  if (SUCCEEDED(hr))
    s.acquired = false;
  return hr;
}
} // namespace deskflow::audio::windows
