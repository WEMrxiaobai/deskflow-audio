// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Compile the real Session implementation into this explicit hardware-test EXE
// with ONLY its exclusivity mutex in a test-specific namespace. Never relax the
// installed product's guard or stop the user's active Deskflow session.
#include <winsock2.h>
#include <windows.h>
#include <string>
namespace {
HANDLE testSessionMutex(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
  const std::wstring isolated = L"Local\\DeskflowAudioSenderWidgetTest-" + std::to_wstring(GetCurrentProcessId());
  return ::CreateMutexW(attributes, owner, name ? isolated.c_str() : nullptr);
}
}
#define CreateMutexW testSessionMutex
#include "../../../apps/deskflow-audio-preview/Session.cpp"
#undef CreateMutexW
