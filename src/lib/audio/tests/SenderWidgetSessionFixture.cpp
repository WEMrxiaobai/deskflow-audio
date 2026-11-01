// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Compile the real Session implementation into this explicit hardware-test EXE
// with ONLY its exclusivity mutex in a test-specific namespace. Never relax the
// installed product's guard or stop the user's active Deskflow session.
#include "../../../apps/deskflow-audio-preview/Session.h"
#include <string>
#include <windows.h>
#include <winsock2.h>
namespace {
std::atomic<bool> failSenderReceive{false};
thread_local bool receivingSession = false;
int fixtureRecvFrom(SOCKET socket, char *buffer, int length, int flags, sockaddr *from, int *fromLength)
{
  if (!receivingSession && failSenderReceive.exchange(false)) {
    WSASetLastError(WSAENETDOWN);
    return SOCKET_ERROR;
  }
  return ::recvfrom(socket, buffer, length, flags, from, fromLength);
}
} // namespace
void failSenderWidgetReceive()
{
  failSenderReceive = true;
}
namespace {
HANDLE testSessionMutex(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
  const std::wstring isolated = L"Local\\DeskflowAudioSenderWidgetTest-" + std::to_wstring(GetCurrentProcessId());
  return ::CreateMutexW(attributes, owner, name ? isolated.c_str() : nullptr);
}
} // namespace
#define CreateMutexW testSessionMutex
#define recvfrom fixtureRecvFrom
#define runSession runSessionImpl
#include "../../../apps/deskflow-audio-preview/Session.cpp"
#undef runSession
#undef recvfrom
#undef CreateMutexW
namespace preview {
void runSession(const SessionOptions &options, std::atomic<bool> &stop, const SessionObserver &observer)
{
  receivingSession = options.receive;
  runSessionImpl(options, stop, observer);
}
} // namespace preview
