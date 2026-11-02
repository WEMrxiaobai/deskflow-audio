// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Inject a single receiver socket failure into the production Session code.
// The other source and the authenticated TLS control connections stay alive.
#include "../../../apps/deskflow-audio-preview/Session.h"
#include <windows.h>
#include <winsock2.h>
namespace {
std::atomic<int> failingSlot{-1};
std::atomic<int> failedRestore{0};
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
void failGroupRestore(int error)
{
  failedRestore = error;
}
#define recvfrom fixtureRecvFrom
#define runSession runSessionImpl
#define restoreLocalOutputState restoreLocalOutputStateImpl
#include "../../../apps/deskflow-audio-preview/Session.cpp"
#undef restoreLocalOutputState
#undef runSession
#undef recvfrom
namespace preview {
bool restoreLocalOutputState(const std::shared_ptr<LocalOutputState> &state) noexcept
{
  if (failedRestore.exchange(0))
    return false; // Inject both a failed HRESULT and a timeout without touching an endpoint.
  return restoreLocalOutputStateImpl(state);
}
void runSession(const SessionOptions &options, std::atomic<bool> &stop, const SessionObserver &observer)
{
  receivingSlot = options.sharedPlayback ? static_cast<int>(options.playbackSlot) : -1;
  runSessionImpl(options, stop, observer);
}
} // namespace preview
