// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Exercise production session exclusion in a process-local test namespace,
// without colliding with an already-running Deskflow on the developer's desktop.
#include <winsock2.h>
#include <windows.h>
#include <string>
namespace {
HANDLE testSessionMutex(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
  const auto isolated = L"Local\\DeskflowAudioSessionTest-" + std::to_wstring(GetCurrentProcessId());
  return ::CreateMutexW(attributes, owner, name ? isolated.c_str() : nullptr);
}
} // namespace
#define CreateMutexW testSessionMutex
#include "Session.cpp"
#undef CreateMutexW
