// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Inject a single receiver socket failure into the production Session code.
// The other source and the authenticated TLS control connections stay alive.
#include "../../../apps/deskflow-audio-preview/Session.h"
#include <windows.h>
#include <winsock2.h>
namespace {
std::atomic<int> failingSlot{-1};
thread_local int receivingSlot = -1;
int fixtureRecvFrom(SOCKET socket, char *buffer, int length, int flags, sockaddr *from, int *fromLength)
{
  int expected = receivingSlot;
  if (expected >= 0 && failingSlot.compare_exchange_strong(expected, -1)) {
    WSASetLastError(WSAENETDOWN);
    return SOCKET_ERROR;
  }
  return ::recvfrom(socket, buffer, length, flags, from, fromLength);
}
} // namespace
void failGroupReceiver(std::size_t slot)
{
  failingSlot = static_cast<int>(slot);
}
#define recvfrom fixtureRecvFrom
#define runSession runSessionImpl
#include "../../../apps/deskflow-audio-preview/Session.cpp"
#undef runSession
#undef recvfrom
namespace preview {
void runSession(const SessionOptions &options, std::atomic<bool> &stop, const SessionObserver &observer)
{
  receivingSlot = options.sharedPlayback ? static_cast<int>(options.playbackSlot) : -1;
  runSessionImpl(options, stop, observer);
}
} // namespace preview
