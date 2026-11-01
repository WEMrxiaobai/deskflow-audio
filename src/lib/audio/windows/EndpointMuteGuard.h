/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <endpointvolume.h>
#include <memory>
#include <string>

namespace deskflow::audio::windows {

// Qt-free, non-real-time control-worker object. Initialize/use/destroy on ONE
// COM-initialized thread, before CoUninitialize. Never call from audio callbacks.
// The registered callback owns its state (not a pointer to this guard); OnNotify
// does only GUID comparisons and lock-free atomics: no I/O, locks or COM calls.
class EndpointMuteGuard final
{
public:
  // Injection seam for deterministic tests. Native implementation touches only
  // GetMute/SetMute; no volume or default-device writes are ever performed.
  class Endpoint
  {
  public:
    virtual ~Endpoint() = default;
    virtual HRESULT getMute(BOOL &muted) noexcept = 0;
    virtual HRESULT setMute(BOOL muted, const GUID &context) noexcept = 0;
    virtual HRESULT subscribe(IAudioEndpointVolumeCallback *callback) noexcept = 0;
    virtual HRESULT unsubscribe(IAudioEndpointVolumeCallback *callback) noexcept = 0;
  };

  EndpointMuteGuard() noexcept;
  ~EndpointMuteGuard();
  EndpointMuteGuard(const EndpointMuteGuard &) = delete;
  EndpointMuteGuard &operator=(const EndpointMuteGuard &) = delete;

  // Null/empty ID resolves default eRender/eConsole ONCE. An exact ID never
  // resolves the default. Initialization snapshots original mute, but does not
  // change it. A guard may be initialized only once; construct a new one to rebind.
  HRESULT initialize(const wchar_t *exactEndpointId = nullptr) noexcept;
  HRESULT initialize(std::unique_ptr<Endpoint> endpoint, std::wstring exactEndpointId) noexcept;
  const std::wstring &endpointId() const noexcept;

  // false: acquire mute; true: safely restore our mute. Suitable for a 100ms
  // control tick. Any external-context notification permanently forfeits this
  // instance's ownership: S_FALSE, no force/reacquisition/restoration thereafter.
  // Initially-muted endpoints are NEVER unmuted. Own GUID is unique per guard
  // and recognized even for synchronous or arbitrarily delayed notifications.
  HRESULT update(bool localPlayback) noexcept;
  HRESULT restore() noexcept;
  bool ownershipLost() const noexcept;
  bool mutedByUs() const noexcept;

  // WASAPI has no compare-and-set. An external change in the final GetMute ->
  // SetMute window cannot be made atomic; notifications prevent all subsequent
  // writes, but cannot retroactively cancel an already-entered SetMute call.
  // Destructor is best effort only: use restore() to observe its HRESULT. A
  // process crash cannot run destructors; use the independent MuteWatchdog.
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::audio::windows
