/*
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Self-contained fake endpoint tests: never open or modify an audio device.
#include "../EndpointMuteGuard.h"
#include <cstdio>
#include <memory>
#include <thread>

using deskflow::audio::windows::EndpointMuteGuard;
namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { std::printf("FAIL line %d: %s\n", __LINE__, #expression); ++failures; } } while (false)
struct Fake final : EndpointMuteGuard::Endpoint
{
  BOOL muted = FALSE;
  HRESULT readResult = S_OK, writeResult = S_OK;
  int reads = 0, writes = 0;
  bool synchronousOwn = true, externalInSet = false, externalInGet = false;
  GUID lastContext{};
  BOOL *finalMute = nullptr;
  int *finalWrites = nullptr;
  IAudioEndpointVolumeCallback *callback = nullptr;
  ~Fake() override
  {
    if (finalMute) *finalMute = muted;
    if (finalWrites) *finalWrites = writes;
    if (callback) callback->Release();
  }
  HRESULT getMute(BOOL &value) noexcept override
  {
    ++reads;
    if (FAILED(readResult)) return readResult;
    value = muted;
    if (externalInGet) { externalInGet = false; notify(GUID{}); }
    return readResult;
  }
  HRESULT setMute(BOOL value, const GUID &context) noexcept override
  {
    ++writes;
    if (FAILED(writeResult)) return writeResult;
    muted = value;
    lastContext = context;
    if (synchronousOwn) notify(context);
    if (externalInSet) { muted = FALSE; notify(GUID{}); }
    return writeResult;
  }
  HRESULT subscribe(IAudioEndpointVolumeCallback *value) noexcept override
  {
    callback = value; callback->AddRef(); return S_OK;
  }
  HRESULT unsubscribe(IAudioEndpointVolumeCallback *value) noexcept override
  {
    CHECK(callback == value);
    callback->Release(); callback = nullptr; return S_OK;
  }
  void notify(const GUID &context) noexcept
  {
    AUDIO_VOLUME_NOTIFICATION_DATA notification{};
    notification.guidEventContext = context;
    notification.bMuted = muted;
    if (callback) (void)callback->OnNotify(&notification);
  }
};
void normal()
{
  auto endpoint = std::make_unique<Fake>(); auto *f = endpoint.get();
  EndpointMuteGuard guard;
  CHECK(guard.initialize(std::move(endpoint), L"exact-A") == S_OK);
  CHECK(guard.endpointId() == L"exact-A");
  CHECK(guard.update(false) == S_OK && f->muted && guard.mutedByUs());
  CHECK(guard.update(false) == S_OK && f->writes == 1);
  CHECK(!guard.ownershipLost()); // synchronous own callback is not external
  CHECK(guard.update(true) == S_OK && !f->muted && f->writes == 2);
  CHECK(guard.restore() == S_OK && f->writes == 2);
  CHECK(guard.update(false) == S_OK && f->writes == 3);
  CHECK(guard.restore() == S_OK && f->writes == 4);
}
void initiallyMuted()
{
  auto endpoint = std::make_unique<Fake>(); auto *f = endpoint.get(); f->muted = TRUE;
  EndpointMuteGuard guard;
  CHECK(guard.initialize(std::move(endpoint), L"exact-B") == S_OK);
  CHECK(guard.update(false) == S_OK);
  CHECK(guard.update(true) == S_OK && f->muted && f->writes == 0);
  CHECK(guard.restore() == S_OK && f->writes == 0);
}
void outsideChanges()
{
  for (BOOL value : {FALSE, TRUE}) {
    auto endpoint = std::make_unique<Fake>(); auto *f = endpoint.get();
    EndpointMuteGuard guard;
    CHECK(guard.initialize(std::move(endpoint), L"exact-C") == S_OK);
    CHECK(guard.update(false) == S_OK);
    f->muted = value; f->notify(GUID{});
    CHECK(guard.ownershipLost() && !guard.mutedByUs());
    CHECK(guard.update(false) == S_FALSE && f->writes == 1);
    CHECK(guard.update(true) == S_FALSE && f->writes == 1 && f->muted == value);
    CHECK(guard.restore() == S_FALSE && f->writes == 1);
  }
}
void delayedAndRacingCallbacks()
{
  auto endpoint = std::make_unique<Fake>(); auto *f = endpoint.get(); f->synchronousOwn = false;
  EndpointMuteGuard guard;
  CHECK(guard.initialize(std::move(endpoint), L"exact-D") == S_OK);
  CHECK(guard.update(false) == S_OK);
  const GUID own = f->lastContext;
  CHECK(guard.restore() == S_OK);
  f->notify(own); // delayed old SetMute callback, outside the SetMute call
  CHECK(!guard.ownershipLost());
  f->externalInSet = true;
  CHECK(guard.update(false) == S_OK);
  CHECK(guard.ownershipLost());
  f->notify(own); // late own event must NEVER erase the external lost flag
  CHECK(guard.restore() == S_FALSE && f->writes == 3 && !f->muted);
}
void failureAndReadRaces()
{
  auto endpoint = std::make_unique<Fake>(); auto *f = endpoint.get();
  EndpointMuteGuard guard;
  CHECK(guard.initialize(std::move(endpoint), L"exact-E") == S_OK);
  f->readResult = E_ACCESSDENIED;
  CHECK(guard.update(false) == E_ACCESSDENIED && f->writes == 0);
  f->readResult = S_OK; f->writeResult = E_FAIL;
  CHECK(guard.update(false) == E_FAIL && !guard.mutedByUs());
  f->writeResult = S_OK;
  CHECK(guard.update(false) == S_OK && guard.mutedByUs());
  f->writeResult = E_ACCESSDENIED;
  CHECK(guard.restore() == E_ACCESSDENIED && guard.mutedByUs());
  f->writeResult = S_OK;
  CHECK(guard.restore() == S_OK && !f->muted);
  const int writes = f->writes;
  f->externalInGet = true;
  CHECK(guard.update(false) == S_FALSE && f->writes == writes);
}
void destructorAndLateCallback()
{
  BOOL finalMute = TRUE;
  int finalWrites = 0;
  IAudioEndpointVolumeCallback *late = nullptr;
  {
    auto endpoint = std::make_unique<Fake>(); auto *f = endpoint.get();
    f->finalMute = &finalMute; f->finalWrites = &finalWrites;
    EndpointMuteGuard guard;
    CHECK(guard.initialize(std::move(endpoint), L"exact-destructor") == S_OK);
    CHECK(guard.update(false) == S_OK);
    late = f->callback;
    late->AddRef();
  }
  CHECK(finalMute == FALSE && finalWrites == 2);
  AUDIO_VOLUME_NOTIFICATION_DATA notification{};
  CHECK(late->OnNotify(&notification) == S_OK); // cannot access destroyed guard
  late->Release();
}
void missedNotificationAndThread()
{
  auto endpoint = std::make_unique<Fake>(); auto *f = endpoint.get();
  EndpointMuteGuard guard;
  CHECK(guard.initialize(std::move(endpoint), L"exact-F") == S_OK);
  HRESULT wrongThread = S_OK;
  std::thread thread([&] { wrongThread = guard.update(false); }); thread.join();
  CHECK(wrongThread == RPC_E_WRONG_THREAD && f->writes == 0);
  f->muted = TRUE; // external write before its callback arrives
  CHECK(guard.update(false) == S_FALSE && guard.ownershipLost() && f->writes == 0);
}
} // namespace
int main()
{
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(apartment)) return 2;
  normal(); initiallyMuted(); outsideChanges(); delayedAndRacingCallbacks();
  failureAndReadRaces(); missedNotificationAndThread(); destructorAndLateCallback();
  CoUninitialize();
  std::printf("EndpointMuteGuard: %d failures (fake endpoints; no device writes)\n", failures);
  return failures ? 1 : 0;
}
