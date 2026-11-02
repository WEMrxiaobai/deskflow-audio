// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Wtsapi32.h>
#include <QLocalSocket>

namespace deskflow::core::ipc::detail {
enum class AccessServer { Core, Daemon };

// Pure policy, kept separate from OS queries so failures, console and RDP
// sessions can be exercised without changing the user's logged-in sessions.
inline bool allowSession(AccessServer server, DWORD serverSession, DWORD clientSession, bool active, bool localSystem)
{
  if (server == AccessServer::Core)
    return serverSession == clientSession;
  return active || (clientSession == 0 && localSystem);
}

inline bool clientIsLocalSystem(DWORD pid)
{
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return false;
  HANDLE token = nullptr;
  const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
  CloseHandle(process);
  if (!opened)
    return false;
  alignas(TOKEN_USER) BYTE buffer[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)]{};
  DWORD used = 0;
  const bool system = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &used) &&
                      IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid, WinLocalSystemSid);
  CloseHandle(token);
  return system;
}

inline bool authorizePipe(QLocalSocket *socket, AccessServer server)
{
  ULONG pid = 0;
  DWORD clientSession = 0, serverSession = 0;
  const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
  if (!GetNamedPipeClientProcessId(pipe, &pid) || !pid || !ProcessIdToSessionId(pid, &clientSession) ||
      !ProcessIdToSessionId(GetCurrentProcessId(), &serverSession))
    return false;
  if (server == AccessServer::Core)
    return allowSession(server, serverSession, clientSession, false, false);
  if (clientSession == 0)
    return allowSession(server, serverSession, clientSession, false, clientIsLocalSystem(pid));
  LPWSTR info = nullptr;
  DWORD bytes = 0;
  const bool queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, clientSession, WTSConnectState, &info, &bytes);
  const bool active = queried && info && bytes >= sizeof(WTS_CONNECTSTATE_CLASS) &&
                      *reinterpret_cast<WTS_CONNECTSTATE_CLASS *>(info) == WTSActive;
  if (info)
    WTSFreeMemory(info);
  return allowSession(server, serverSession, clientSession, active, false);
}
} // namespace deskflow::core::ipc::detail
#endif
