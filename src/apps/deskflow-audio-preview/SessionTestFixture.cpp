// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Exercise production session exclusion in a process-local test namespace,
// without colliding with an already-running Deskflow on the developer's desktop.
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
std::atomic<bool> holdPrepare{false}, prepareEntered{false};
void sessionPrepareHook(const std::atomic<bool> &stop)
{
  prepareEntered = true;
  while (holdPrepare && !stop)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
namespace {
HANDLE testSessionMutex(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
  const auto isolated = L"Local\\DeskflowAudioSessionTest-" + std::to_wstring(GetCurrentProcessId());
  return ::CreateMutexW(attributes, owner, name ? isolated.c_str() : nullptr);
}
} // namespace
#define CreateMutexW testSessionMutex
#define DESKFLOW_SESSION_TEST_PREPARE_HOOK
#include "Session.cpp"
#undef CreateMutexW
