// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Exercise the real helper protocol with an injected endpoint guard. No audio
// endpoint or user volume is accessed by this test.
#include <windows.h>
#include <endpointvolume.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include "../windows/EndpointMuteGuard.h"
#include "../windows/MuteWatchdog.h"
namespace {
std::atomic<int> restores{0}, failRestore{0};
class FakeMuteGuard
{
public:
  ~FakeMuteGuard() { (void)restore(); }
  HRESULT initialize(const wchar_t *) noexcept { return S_OK; }
  HRESULT update(bool) noexcept { return S_OK; }
  HRESULT restore() noexcept
  {
    ++restores;
    return failRestore.exchange(0) ? E_FAIL : S_OK;
  }
};
}
#define EndpointMuteGuard FakeMuteGuard
#define wmain watchdogFixtureMain
#include "../windows/MuteWatchdogMain.cpp"
#undef wmain
#undef EndpointMuteGuard

int main()
{
  using namespace deskflow::audio::windows::mute_watchdog_detail;
  auto exercise = [](int mode) {
    HANDLE inputRead = nullptr, inputWrite = nullptr, outputRead = nullptr, outputWrite = nullptr;
    HANDLE parent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!parent || !CreatePipe(&inputRead, &inputWrite, nullptr, 0) ||
        !CreatePipe(&outputRead, &outputWrite, nullptr, 0))
      return false;
    restores = 0;
    failRestore = 0;
    int result = -1;
    std::thread helper([&] { result = run(inputRead, outputWrite, parent); });
    const Command init{Operation::Initialize, 1};
    const wchar_t id = L'x';
    DWORD count = 0;
    bool ok = WriteFile(inputWrite, &init, sizeof(init), &count, nullptr) && count == sizeof(init) &&
              WriteFile(inputWrite, &id, sizeof(id), &count, nullptr) && count == sizeof(id);
    Reply reply{};
    ok = ok && ReadFile(outputRead, &reply, sizeof(reply), &count, nullptr) && count == sizeof(reply) &&
         reply.result == S_OK;
    if (mode == 0) {
      // Explicit Quit returns the restoration failure; destructor still retries.
      failRestore = 1;
      const Command quit{Operation::Quit, 0};
      ok = ok && WriteFile(inputWrite, &quit, sizeof(quit), &count, nullptr) && count == sizeof(quit) &&
           ReadFile(outputRead, &reply, sizeof(reply), &count, nullptr) && count == sizeof(reply) &&
           reply.result == E_FAIL;
    } else if (mode == 1) {
      CloseHandle(inputWrite);
      inputWrite = nullptr; // EOF must restore.
    } else {
      SetEvent(parent); // Parent death must restore even if the pipe stays open.
    }
    helper.join();
    ok = ok && restores >= 2 && result == (mode == 0 ? 6 : 0);
    if (inputWrite) CloseHandle(inputWrite);
    CloseHandle(inputRead);
    CloseHandle(outputRead);
    CloseHandle(outputWrite);
    CloseHandle(parent);
    return ok;
  };
  const bool ok = exercise(0) && exercise(1) && exercise(2);
  std::puts(ok ? "mute helper failure, EOF and parent exit: PASS" : "mute helper protocol: FAIL");
  return ok ? 0 : 1;
}
