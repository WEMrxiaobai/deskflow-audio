// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QtGlobal>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QString>
#include <vector>

namespace deskflow::gui::ipc::detail {
struct LookupDiagnostic {
  const char *stage = nullptr;
  DWORD error = 0;
};

inline DWORD registeredDaemonPid(LookupDiagnostic &diagnostic)
{
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) { diagnostic = {"open service manager", GetLastError()}; return 0; }
  SC_HANDLE service = OpenServiceW(manager, L"Deskflow", SERVICE_QUERY_STATUS);
  if (!service) {
    diagnostic = {"open Deskflow service", GetLastError()};
    CloseServiceHandle(manager);
    return 0;
  }
  SERVICE_STATUS_PROCESS status{};
  DWORD size = 0;
  const bool ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
      reinterpret_cast<BYTE *>(&status), sizeof(status), &size);
  if (!ok) diagnostic = {"query Deskflow service", GetLastError()};
  else if (status.dwCurrentState != SERVICE_RUNNING) diagnostic = {"Deskflow service not running", 0};
  else if (!status.dwProcessId) diagnostic = {"Deskflow service PID unavailable", 0};
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return ok && status.dwCurrentState == SERVICE_RUNNING ? status.dwProcessId : 0;
}
inline DWORD parentProcessId(DWORD pid, LookupDiagnostic &diagnostic)
{
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) { diagnostic = {"create process snapshot", GetLastError()}; return 0; }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD parent = 0;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == pid) { parent = entry.th32ParentProcessID; break; }
    } while (Process32NextW(snapshot, &entry));
    if (!parent) diagnostic = {"find server parent", ERROR_NOT_FOUND};
  } else {
    diagnostic = {"enumerate process snapshot", GetLastError()};
  }
  CloseHandle(snapshot);
  return parent;
}
enum class PeerFailure {
  None, PipeServerPid, ServerSession, ClientSession, SessionMismatch,
  OpenServerProcess, QueryServerImage, ResolveServerImage, ResolveExpectedImage, ExecutableMismatch
};

struct PeerVerification {
  PeerFailure failure = PeerFailure::None;
  DWORD error = 0;
  LookupDiagnostic daemonLookup{};
  LookupDiagnostic parentLookup{};
  bool verified() const { return failure == PeerFailure::None; }
};

inline const char *peerFailureName(PeerFailure failure)
{
  switch (failure) {
  case PeerFailure::None: return "none";
  case PeerFailure::PipeServerPid: return "pipe server PID";
  case PeerFailure::ServerSession: return "server session lookup";
  case PeerFailure::ClientSession: return "client session lookup";
  case PeerFailure::SessionMismatch: return "session mismatch";
  case PeerFailure::OpenServerProcess: return "open server process";
  case PeerFailure::QueryServerImage: return "query server image";
  case PeerFailure::ResolveServerImage: return "resolve server image";
  case PeerFailure::ResolveExpectedImage: return "resolve expected image";
  case PeerFailure::ExecutableMismatch: return "executable mismatch";
  }
  return "unknown";
}

struct WindowsPeerOperations {
  HANDLE pipe;
  bool serverPid(ULONG &pid) const { return GetNamedPipeServerProcessId(pipe, &pid); }
  DWORD daemonPid(LookupDiagnostic &diagnostic) const { return registeredDaemonPid(diagnostic); }
  DWORD parentPid(DWORD pid, LookupDiagnostic &diagnostic) const { return parentProcessId(pid, diagnostic); }
  bool session(DWORD pid, DWORD &id) const { return ProcessIdToSessionId(pid, &id); }
  DWORD currentPid() const { return GetCurrentProcessId(); }
  DWORD error() const { return GetLastError(); }
  PeerVerification executable(DWORD pid, const QString &expected) const
  {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {PeerFailure::OpenServerProcess, GetLastError()};
    std::vector<wchar_t> name(32768);
    DWORD size = static_cast<DWORD>(name.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, name.data(), &size);
    const DWORD queryError = ok ? 0 : GetLastError();
    CloseHandle(process);
    if (!ok) return {PeerFailure::QueryServerImage, queryError};
    const auto actual = QFileInfo(QString::fromWCharArray(name.data(), size)).canonicalFilePath();
    if (actual.isEmpty()) return {PeerFailure::ResolveServerImage, 0};
    const auto wanted = QFileInfo(expected).canonicalFilePath();
    if (wanted.isEmpty()) return {PeerFailure::ResolveExpectedImage, 0};
    if (actual.compare(wanted, Qt::CaseInsensitive) != 0) return {PeerFailure::ExecutableMismatch, 0};
    return {};
  }
};

// Operations are injectable so every rejection can be exercised without a service or pipe.
template <typename Operations>
PeerVerification verifyPipeServerWith(const Operations &ops, bool daemon, const QString &corePath)
{
  ULONG pid = 0;
  if (!ops.serverPid(pid)) return {PeerFailure::PipeServerPid, ops.error()};
  if (!pid) return {PeerFailure::PipeServerPid, 0};
  LookupDiagnostic daemonLookup, parentLookup;
  const DWORD registered = ops.daemonPid(daemonLookup);
  if (daemon && registered && pid == registered) return {};
  auto failure = [&](PeerFailure reason, DWORD error = 0) {
    return PeerVerification{reason, error, daemonLookup, parentLookup};
  };
  DWORD serverSession = 0, clientSession = 0;
  if (!ops.session(pid, serverSession)) return failure(PeerFailure::ServerSession, ops.error());
  if (!ops.session(ops.currentPid(), clientSession)) return failure(PeerFailure::ClientSession, ops.error());
  if (serverSession != clientSession) return failure(PeerFailure::SessionMismatch);
  if (!daemon && registered && ops.parentPid(pid, parentLookup) == registered) return {};
  const auto expected = daemon ? QFileInfo(corePath).dir().filePath(QStringLiteral("deskflow-daemon.exe")) : corePath;
  auto result = ops.executable(pid, expected);
  result.daemonLookup = daemonLookup;
  result.parentLookup = parentLookup;
  return result;
}

inline PeerVerification verifyPipeServer(QLocalSocket *socket, bool daemon, const QString &corePath)
{
  return verifyPipeServerWith(WindowsPeerOperations{reinterpret_cast<HANDLE>(socket->socketDescriptor())}, daemon, corePath);
}
}
#endif
