/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "MuteWatchdog.h"
#include <array>
#include <new>
#include <vector>

namespace deskflow::audio::windows {
namespace {
using namespace mute_watchdog_detail;
class Handle final
{
public:
  Handle() noexcept = default;
  explicit Handle(HANDLE value) noexcept : value(value)
  {
  }
  ~Handle()
  {
    reset();
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  void reset(HANDLE next = nullptr) noexcept
  {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
    value = next;
  }
  HANDLE value = nullptr;
};
HRESULT winError() noexcept
{
  const DWORD error = GetLastError();
  return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
HRESULT writeBytes(HANDLE pipe, const void *data, DWORD bytes) noexcept
{
  DWORD written = 0;
  if (!WriteFile(pipe, data, bytes, &written, nullptr))
    return winError();
  return written == bytes ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
}
HRESULT readReply(HANDLE pipe, HANDLE process) noexcept
{
  const ULONGLONG deadline = GetTickCount64() + 3000;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
      return winError();
    if (available >= sizeof(Reply)) {
      Reply reply{};
      DWORD read = 0;
      if (!ReadFile(pipe, &reply, sizeof(reply), &read, nullptr))
        return winError();
      if (read != sizeof(reply) || reply.version != protocolVersion)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      return reply.result;
    }
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
      return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    if (GetTickCount64() >= deadline)
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    Sleep(5); // control worker only; never an audio callback
  }
}
} // namespace

struct MuteWatchdog::Impl
{
  Handle input, output, process;
  bool healthy = false;
  HRESULT lastFailure = S_OK;
  HRESULT command(mute_watchdog_detail::Operation operation) noexcept
  {
    const mute_watchdog_detail::Command command{operation, 0};
    HRESULT hr = writeBytes(input.value, &command, sizeof(command));
    if (SUCCEEDED(hr))
      hr = readReply(output.value, process.value);
    if (FAILED(hr)) {
      lastFailure = hr;
      healthy = false;
      // EOF is the independent helper's cleanup signal. NEVER TerminateProcess:
      // killing the only owner would prevent restoration.
      input.reset();
      output.reset();
    }
    return hr;
  }
};

MuteWatchdog::MuteWatchdog() noexcept : m_impl(new(std::nothrow) Impl)
{
}
MuteWatchdog::~MuteWatchdog()
{
  (void)stop();
}
bool MuteWatchdog::running() const noexcept
{
  return m_impl && m_impl->healthy && m_impl->process.value &&
         WaitForSingleObject(m_impl->process.value, 0) == WAIT_TIMEOUT;
}

HRESULT MuteWatchdog::start(const std::wstring &exactEndpointId) noexcept
{
  using namespace mute_watchdog_detail;
  if (!m_impl)
    return E_OUTOFMEMORY;
  if (m_impl->process.value)
    return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
  if (exactEndpointId.empty() || exactEndpointId.size() >= maximumIdCharacters ||
      exactEndpointId.find(L'\0') != std::wstring::npos)
    return E_INVALIDARG;
  try {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size())
      return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::wstring executable(path.data(), size);
    const auto slash = executable.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
      return E_UNEXPECTED;
    executable.resize(slash + 1);
    executable += L"deskflow-audio-mute-watchdog.exe";

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle childInput, parentInput, parentOutput, childOutput, parentWait;
    if (!CreatePipe(&childInput.value, &parentInput.value, &security, 32768) ||
        !CreatePipe(&parentOutput.value, &childOutput.value, &security, 32768))
      return winError();
    if (!SetHandleInformation(parentInput.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parentOutput.value, HANDLE_FLAG_INHERIT, 0))
      return winError();
    if (!DuplicateHandle(
            GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parentWait.value, SYNCHRONIZE, TRUE, 0
        ))
      return winError();

    SIZE_T attributeBytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributes(attributeBytes);
    auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attributeBytes))
      return winError();
    struct AttributeCleanup
    {
      LPPROC_THREAD_ATTRIBUTE_LIST list;
      ~AttributeCleanup()
      {
        DeleteProcThreadAttributeList(list);
      }
    } cleanup{list};
    std::array<HANDLE, 3> inherited{childInput.value, childOutput.value, parentWait.value};
    if (!UpdateProcThreadAttribute(
            list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr
        ))
      return winError();
    auto number = [](HANDLE handle) { return std::to_wstring(reinterpret_cast<ULONG_PTR>(handle)); };
    std::wstring command = L"\"" + executable + L"\" " + number(childInput.value) + L" " + number(childOutput.value) +
                           L" " + number(parentWait.value);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(
            executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &info
        ))
      return winError();
    Handle thread(info.hThread);
    childInput.reset();
    childOutput.reset();
    parentWait.reset();
    auto &s = *m_impl;
    s.process.reset(info.hProcess);
    s.input.reset(parentInput.value);
    parentInput.value = nullptr;
    s.output.reset(parentOutput.value);
    parentOutput.value = nullptr;
    const Command initialize{Operation::Initialize, static_cast<DWORD>(exactEndpointId.size())};
    HRESULT hr = writeBytes(s.input.value, &initialize, sizeof(initialize));
    if (SUCCEEDED(hr))
      hr = writeBytes(s.input.value, exactEndpointId.data(), initialize.characters * sizeof(wchar_t));
    if (SUCCEEDED(hr))
      hr = readReply(s.output.value, s.process.value);
    if (FAILED(hr)) {
      s.input.reset();
      s.output.reset();
      s.process.reset();
      return hr;
    }
    s.healthy = true;
    return hr;
  } catch (...) {
    return E_OUTOFMEMORY;
  }
}

HRESULT MuteWatchdog::update(bool localPlayback) noexcept
{
  if (!running())
    return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
  // Send each control tick so an external change is reported as S_FALSE even
  // if localPlayback did not change. Guard itself performs no repeated SetMute.
  return m_impl->command(
      localPlayback ? mute_watchdog_detail::Operation::Restore : mute_watchdog_detail::Operation::Mute
  );
}
HRESULT MuteWatchdog::stop() noexcept
{
  if (!m_impl || !m_impl->process.value)
    return S_OK;
  auto &s = *m_impl;
  HRESULT hr = S_OK;
  if (running())
    hr = s.command(mute_watchdog_detail::Operation::Quit);
  else
    hr = FAILED(s.lastFailure) ? s.lastFailure : HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
  s.input.reset();
  s.output.reset();
  // Even on timeout, closing the command pipe requests safe restoration. The
  // child remains independent and watches parent death if COM is temporarily stuck.
  const DWORD exited = WaitForSingleObject(s.process.value, 3000);
  if (exited != WAIT_OBJECT_0 && SUCCEEDED(hr))
    hr = exited == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : winError();
  s.process.reset();
  s.healthy = false;
  return hr;
}
} // namespace deskflow::audio::windows
