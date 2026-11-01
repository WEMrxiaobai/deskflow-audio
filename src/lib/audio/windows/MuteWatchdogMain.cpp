/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "EndpointMuteGuard.h"
#include "MuteWatchdog.h"
#include <cerrno>
#include <cwchar>
#include <string>

namespace {
using namespace deskflow::audio::windows;
using namespace deskflow::audio::windows::mute_watchdog_detail;
struct Handle
{
  HANDLE value = nullptr;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Apartment
{
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
HANDLE parseHandle(const wchar_t *argument) noexcept
{
  if (!argument || *argument < L'0' || *argument > L'9')
    return nullptr;
  wchar_t *end = nullptr;
  errno = 0;
  const auto number = std::wcstoull(argument, &end, 10);
  if (errno || !end || *end || !number || number > static_cast<unsigned long long>(~ULONG_PTR{0}))
    return nullptr;
  return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(number));
}
// Anonymous pipes cannot be waited on for readability. Peek before each bounded
// read, while waiting on the actual inherited parent process handle every 10ms.
// Parent crash/EOF is therefore observed even with no outstanding commands.
bool readBytes(HANDLE pipe, HANDLE parent, void *destination, DWORD bytes) noexcept
{
  auto *out = static_cast<unsigned char *>(destination);
  DWORD remaining = bytes;
  while (remaining) {
    if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT)
      return false;
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
      return false;
    if (!available) {
      if (WaitForSingleObject(parent, 10) != WAIT_TIMEOUT)
        return false;
      continue;
    }
    const DWORD wanted = available < remaining ? available : remaining;
    DWORD received = 0;
    if (!ReadFile(pipe, out, wanted, &received, nullptr) || !received)
      return false;
    out += received;
    remaining -= received;
  }
  return true;
}
bool reply(HANDLE output, HRESULT result) noexcept
{
  const Reply message{protocolVersion, result};
  DWORD written = 0;
  return WriteFile(output, &message, sizeof(message), &written, nullptr) && written == sizeof(message);
}
int run(HANDLE input, HANDLE output, HANDLE parent)
{
  Apartment apartment;
  if (FAILED(apartment.result)) {
    (void)reply(output, apartment.result);
    return 2;
  }
  EndpointMuteGuard guard; // destroyed BEFORE apartment, on every graceful path
  Command command{};
  if (!readBytes(input, parent, &command, sizeof(command)))
    return 3;
  if (command.operation != Operation::Initialize || !command.characters ||
      command.characters >= maximumIdCharacters) {
    (void)reply(output, E_INVALIDARG);
    return 4;
  }
  std::wstring id(command.characters, L'\0');
  if (!readBytes(input, parent, id.data(), command.characters * sizeof(wchar_t)))
    return 3;
  if (id.find(L'\0') != std::wstring::npos) {
    (void)reply(output, E_INVALIDARG);
    return 4;
  }
  const HRESULT initialized = guard.initialize(id.c_str());
  if (!reply(output, initialized) || FAILED(initialized))
    return 5;
  while (readBytes(input, parent, &command, sizeof(command))) {
    if (command.characters) {
      (void)reply(output, E_INVALIDARG);
      return 4;
    }
    HRESULT hr = E_INVALIDARG;
    switch (command.operation) {
    case Operation::Mute: hr = guard.update(false); break;
    case Operation::Restore: hr = guard.restore(); break;
    case Operation::Quit:
      hr = guard.restore();
      (void)reply(output, hr);
      return FAILED(hr) ? 6 : 0;
    default:
      (void)reply(output, E_INVALIDARG);
      return 4;
    }
    if (!reply(output, hr))
      break;
  }
  // On parent death or EOF, make an explicit attempt, then destructor retries a
  // transient failed restoration. Device removal can still make recovery fail.
  return FAILED(guard.restore()) ? 6 : 0;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
  if (argc != 4)
    return 1;
  Handle input{parseHandle(argv[1])}, output{parseHandle(argv[2])}, parent{parseHandle(argv[3])};
  if (!input.value || !output.value || !parent.value ||
      GetFileType(input.value) != FILE_TYPE_PIPE || GetFileType(output.value) != FILE_TYPE_PIPE ||
      WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT)
    return 1;
  try {
    return run(input.value, output.value, parent.value);
  } catch (...) {
    // run's stack unwinds the guard before leaving its COM apartment.
    return 7;
  }
}
