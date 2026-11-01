// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Explicit standalone Windows hardware integration test, NOT CMake/CTest.
// Build C++20 with windows/MuteWatchdog.cpp; link ole32.lib and uuid.lib.
// Place deskflow-audio-mute-watchdog.exe beside this test executable.
// Run: MuteWatchdogTests.exe --manual-watchdog
// Default/unknown arguments never touch audio hardware. Exit 77 = not run/skip.
// No PCM, render/capture, network, microphone, named IPC, or scalar setters.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>
#include "../windows/MuteWatchdog.h"
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using deskflow::audio::windows::MuteWatchdog;
using Microsoft::WRL::ComPtr;
void check(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
void checked(HRESULT hr, const char *message)
{
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s: 0x%08lx\n", message, static_cast<unsigned long>(hr));
    throw std::runtime_error(message);
  }
}
struct Handle
{
  HANDLE value = nullptr;
  ~Handle() { reset(); }
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  void reset(HANDLE next = nullptr)
  {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
    value = next;
  }
};
struct Apartment
{
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Volume
{
  ComPtr<IAudioEndpointVolume> volume;
  std::wstring id;
  explicit Volume(const wchar_t *exact = nullptr)
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    checked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Enumerator");
    ComPtr<IMMDevice> device;
    checked(exact ? enumerator->GetDevice(exact, &device)
                  : enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Resolve endpoint");
    wchar_t *raw = nullptr;
    checked(device->GetId(&raw), "Get endpoint ID");
    try { id = raw; } catch (...) { CoTaskMemFree(raw); throw; }
    CoTaskMemFree(raw);
    checked(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void **>(volume.GetAddressOf())), "Activate volume");
  }
  bool muted() const
  {
    BOOL value = FALSE;
    checked(volume->GetMute(&value), "GetMute");
    return value != FALSE;
  }
  void set(bool value) { checked(volume->SetMute(value ? TRUE : FALSE, nullptr), "Test external SetMute"); }
  float scalar() const
  {
    float value = -1;
    checked(volume->GetMasterVolumeLevelScalar(&value), "GetMasterVolumeLevelScalar");
    return value;
  }
  bool await(bool expected, DWORD timeout = 5000) const
  {
    const ULONGLONG end = GetTickCount64() + timeout;
    do {
      if (muted() == expected)
        return true;
      Sleep(10);
    } while (GetTickCount64() < end);
    return muted() == expected;
  }
};
struct Restore
{
  Volume &endpoint;
  bool original;
  bool &success;
  ~Restore() noexcept
  {
    // This TEST alone deliberately simulates external changes. Regardless of
    // guard ownership, put this exact test endpoint back into its initial state.
    const HRESULT hr = endpoint.volume->SetMute(original ? TRUE : FALSE, nullptr);
    BOOL actual = !original;
    const HRESULT read = endpoint.volume->GetMute(&actual);
    success = SUCCEEDED(hr) && SUCCEEDED(read) && !!actual == original;
    std::printf("TEST FINALLY original_mute=%d restored=%s\n", original, success ? "true" : "false");
  }
};
void writeAll(HANDLE pipe, const void *data, DWORD size)
{
  DWORD done = 0;
  check(WriteFile(pipe, data, size, &done, nullptr) && done == size, "Control pipe write failed");
}
bool readAll(HANDLE pipe, HANDLE peer, void *data, DWORD size, DWORD timeout)
{
  auto *out = static_cast<unsigned char *>(data);
  const ULONGLONG end = GetTickCount64() + timeout;
  while (size) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
      return false;
    if (available) {
      const DWORD wanted = available < size ? available : size;
      DWORD done = 0;
      if (!ReadFile(pipe, out, wanted, &done, nullptr) || !done)
        return false;
      out += done;
      size -= done;
      continue;
    }
    if (WaitForSingleObject(peer, 0) != WAIT_TIMEOUT || GetTickCount64() >= end)
      return false;
    Sleep(5);
  }
  return true;
}
HANDLE parse(const wchar_t *text)
{
  if (!text || *text < L'0' || *text > L'9')
    return nullptr;
  wchar_t *end = nullptr;
  errno = 0;
  const auto value = std::wcstoull(text, &end, 10);
  if (errno || !end || *end || !value || value > static_cast<unsigned long long>(~ULONG_PTR{0}))
    return nullptr;
  return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value));
}
// Private child mode is reachable only with validated inherited anonymous pipe
// handles + a live supervisor process handle. Endpoint ID travels in the pipe,
// not the command line. Child will never alter hardware on invalid invocation.
int childMain(wchar_t **argv)
{
  Handle input(parse(argv[2])), output(parse(argv[3])), supervisor(parse(argv[4]));
  if (!input.value || !output.value || !supervisor.value ||
      GetFileType(input.value) != FILE_TYPE_PIPE || GetFileType(output.value) != FILE_TYPE_PIPE ||
      WaitForSingleObject(supervisor.value, 0) != WAIT_TIMEOUT)
    return 77;
  DWORD count = 0;
  if (!readAll(input.value, supervisor.value, &count, sizeof(count), 3000) || !count || count >= 4096)
    return 1;
  std::wstring id(count, L'\0');
  if (!readAll(input.value, supervisor.value, id.data(), count * sizeof(wchar_t), 3000) ||
      id.find(L'\0') != std::wstring::npos)
    return 1;
  Apartment apartment;
  checked(apartment.result, "Child COM");
  Volume endpoint(id.c_str());
  check(!endpoint.muted(), "Child refuses already-muted baseline");
  MuteWatchdog watchdog;
  checked(watchdog.start(id), "Child watchdog start");
  checked(watchdog.update(false), "Child watchdog mute");
  check(endpoint.await(true), "Child mute readback");
  const DWORD ready = 0x4d555445;
  writeAll(output.value, &ready, sizeof(ready));
  // The test supervisor terminates ONLY this fake parent, never the watchdog.
  // If supervisor exits/pipe closes/timeout occurs, graceful RAII also restores.
  unsigned char command = 0;
  (void)readAll(input.value, supervisor.value, &command, sizeof(command), 30000);
  return 0;
}
void crashParentTest(Volume &endpoint)
{
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  Handle childInput, parentInput, parentOutput, childOutput, supervisor;
  check(CreatePipe(&childInput.value, &parentInput.value, &security, 32768) &&
        CreatePipe(&parentOutput.value, &childOutput.value, &security, 32768), "Create child control pipes");
  check(SetHandleInformation(parentInput.value, HANDLE_FLAG_INHERIT, 0) &&
        SetHandleInformation(parentOutput.value, HANDLE_FLAG_INHERIT, 0), "Limit control pipe inheritance");
  check(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &supervisor.value,
                        SYNCHRONIZE, TRUE, 0), "Duplicate supervisor wait handle");
  SIZE_T bytes = 0;
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> storage(bytes);
  auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  check(InitializeProcThreadAttributeList(list, 1, 0, &bytes), "Initialize child attribute list");
  struct Cleanup { LPPROC_THREAD_ATTRIBUTE_LIST list; ~Cleanup() { DeleteProcThreadAttributeList(list); } } cleanup{list};
  std::array<HANDLE, 3> inherited{childInput.value, childOutput.value, supervisor.value};
  check(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                  sizeof(inherited), nullptr, nullptr), "Set exact inherited handles");
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  check(length && length < path.size(), "Get test executable path");
  const std::wstring executable(path.data(), length);
  auto number = [](HANDLE h) { return std::to_wstring(reinterpret_cast<ULONG_PTR>(h)); };
  std::wstring command = L"\"" + executable + L"\" --child " + number(childInput.value) + L" " +
                         number(childOutput.value) + L" " + number(supervisor.value);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = list;
  PROCESS_INFORMATION process{};
  check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup.StartupInfo, &process), "Launch fake parent test child");
  Handle child(process.hProcess), thread(process.hThread);
  childInput.reset();
  childOutput.reset();
  supervisor.reset();
  // Any exception closes this control pipe and causes graceful child cleanup.
  // Wait for that cleanup before outer TEST FINALLY may restore original mute.
  struct ChildCleanup {
    Handle &input;
    HANDLE process;
    ~ChildCleanup() { input.reset(); (void)WaitForSingleObject(process, 6000); }
  } childCleanup{parentInput, child.value};
  const DWORD count = static_cast<DWORD>(endpoint.id.size());
  writeAll(parentInput.value, &count, sizeof(count));
  writeAll(parentInput.value, endpoint.id.data(), count * sizeof(wchar_t));
  DWORD ready = 0;
  check(readAll(parentOutput.value, child.value, &ready, sizeof(ready), 10000) && ready == 0x4d555445,
        "Fake parent did not report a muted, ready watchdog");
  check(endpoint.muted(), "Supervisor confirms mute before fake parent crash");
  check(TerminateProcess(child.value, 0x54455354), "Terminate ONLY fake test parent");
  check(WaitForSingleObject(child.value, 3000) == WAIT_OBJECT_0, "Fake parent termination timed out");
  check(endpoint.await(false, 6000), "Independent helper did not restore after fake parent crash");
  std::puts("PASS: killed fake parent only; independent watchdog restored endpoint mute.");
}
void run(Volume &endpoint, bool original)
{
  if (original) {
    MuteWatchdog watchdog;
    checked(watchdog.start(endpoint.id), "Already-muted watchdog start");
    checked(watchdog.update(false), "Already-muted update false");
    checked(watchdog.update(true), "Already-muted update true");
    checked(watchdog.stop(), "Already-muted watchdog stop");
    check(endpoint.muted(), "Original muted state must remain muted");
    std::puts("PASS original-muted preservation; other cases SKIP (will not automatically unmute original mute).");
    return;
  }
  {
    MuteWatchdog watchdog;
    checked(watchdog.start(endpoint.id), "Normal watchdog start");
    for (int repeat = 0; repeat < 2; ++repeat) {
      checked(watchdog.update(false), "Normal mute");
      check(endpoint.await(true), "Normal mute readback");
      checked(watchdog.update(true), "Normal restore");
      check(endpoint.await(false), "Normal restore readback");
    }
    checked(watchdog.stop(), "Normal watchdog stop");
  }
  std::puts("PASS: normal repeated mute/unmute toggles.");
  {
    MuteWatchdog watchdog;
    checked(watchdog.start(endpoint.id), "External override watchdog start");
    checked(watchdog.update(false), "Mute before external override");
    check(endpoint.await(true), "Pre-override mute");
    endpoint.set(false); // Simulated user override, NOT a watchdog own-context event.
    Sleep(300); // Allow asynchronous endpoint notification delivery.
    checked(watchdog.update(true), "External override restore must be harmless");
    checked(watchdog.update(false), "External unmute must not be forced back");
    check(!endpoint.muted(), "Watchdog forced mute after external unmute");
    endpoint.set(true); // Later external mute is explicitly user-owned.
    Sleep(300);
  } // Destructor must NOT unmute the later externally owned mute.
  Sleep(300); // Also catch a delayed helper-destructor restoration after Quit reply.
  check(endpoint.muted(), "Destructor undid externally owned mute");
  std::puts("PASS: external unmute not forced back; subsequent external mute survives destructor.");
  endpoint.set(false); // TEST cleanup only, not guard policy.
  Sleep(200);
  {
    endpoint.set(true);
    MuteWatchdog watchdog;
    checked(watchdog.start(endpoint.id), "Explicit initially-muted start");
    checked(watchdog.update(false), "Initially-muted false");
    checked(watchdog.update(true), "Initially-muted true");
    checked(watchdog.stop(), "Initially-muted stop");
    check(endpoint.muted(), "Initially-muted guard unmuted endpoint");
  }
  std::puts("PASS: guard snapshots originally-muted state and preserves it.");
  endpoint.set(false);
  Sleep(200);
  crashParentTest(endpoint);
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
  if (argc == 5 && std::wcscmp(argv[1], L"--child") == 0) {
    try { return childMain(argv); }
    catch (const std::exception &error) { std::fprintf(stderr, "CHILD FAIL: %s\n", error.what()); return 1; }
    catch (...) { return 1; }
  }
  if (argc != 2 || std::wcscmp(argv[1], L"--manual-watchdog") != 0) {
    std::puts("NOT RUN: explicit Windows hardware test. Use --manual-watchdog; never run automatically in CI.");
    return 77;
  }
  std::puts(
      "MANUAL WATCHDOG HARDWARE TEST START: temporarily mutes/unmutes default speakers.\n"
      "This test simulates external user mute changes and crashes ONLY its fake parent child.\n"
      "Original mute is restored in TEST FINALLY; volume scalar is read-only. No PCM/content or named IPC.\n"
      "Do not change endpoint/volume or kill this supervisor; allow cleanup to complete."
  );
  std::fflush(stdout);
  int result = 0;
  bool restored = false;
  try {
    Apartment apartment;
    checked(apartment.result, "Supervisor COM");
    Volume endpoint;
    const bool original = endpoint.muted();
    const float scalar = endpoint.scalar();
    std::printf("ORIGINAL mute=%d raw_scalar=%.9g\n", original, static_cast<double>(scalar));
    {
      Restore restore{endpoint, original, restored};
      try { run(endpoint, original); }
      catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); result = 1; }
      catch (...) { std::fputs("FAIL: unknown exception\n", stderr); result = 1; }
    }
    const float after = endpoint.scalar();
    const bool unchanged = std::isfinite(after) && std::fabs(after - scalar) <= 0.000001f;
    std::printf("FINAL mute_restored=%s raw_scalar=%.9g scalar_unchanged=%s\n", restored ? "true" : "false",
                static_cast<double>(after), unchanged ? "true" : "false");
    if (!restored || !unchanged)
      result = 1;
    if (original && result == 0)
      result = 77; // Preservation passed; destructive/crash scenarios intentionally skipped.
  } catch (const std::exception &error) {
    std::fprintf(stderr, "SETUP FAIL: %s\n", error.what());
    result = 1;
  } catch (...) { result = 1; }
  std::printf("WATCHDOG TEST %s exit_code=%d\n", result == 0 ? "PASS" : result == 77 ? "PARTIAL/SKIP" : "FAIL", result);
  return result;
}
