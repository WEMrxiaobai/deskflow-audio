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
inline DWORD registeredDaemonPid()
{
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return 0;
  SC_HANDLE service = OpenServiceW(manager, L"Deskflow", SERVICE_QUERY_STATUS);
  SERVICE_STATUS_PROCESS status{};
  DWORD size = 0;
  const bool ok = service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
      reinterpret_cast<BYTE *>(&status), sizeof(status), &size) && status.dwCurrentState == SERVICE_RUNNING;
  if (service) CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return ok ? status.dwProcessId : 0;
}
inline DWORD parentProcessId(DWORD pid)
{
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD parent = 0;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == pid) { parent = entry.th32ParentProcessID; break; }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return parent;
}
inline bool sameExecutable(DWORD pid, const QString &expected)
{
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  std::vector<wchar_t> name(32768);
  DWORD size = static_cast<DWORD>(name.size());
  const bool ok = QueryFullProcessImageNameW(process, 0, name.data(), &size);
  CloseHandle(process);
  if (!ok) return false;
  const auto actual = QFileInfo(QString::fromWCharArray(name.data(), size)).canonicalFilePath();
  const auto wanted = QFileInfo(expected).canonicalFilePath();
  return !actual.isEmpty() && !wanted.isEmpty() && actual.compare(wanted, Qt::CaseInsensitive) == 0;
}
inline bool verifyPipeServer(QLocalSocket *socket, bool daemon, const QString &corePath)
{
  ULONG pid = 0;
  const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
  if (!GetNamedPipeServerProcessId(pipe, &pid) || !pid) return false;
  const DWORD registered = registeredDaemonPid();
  if (daemon && registered && pid == registered) return true;
  DWORD serverSession = 0, clientSession = 0;
  if (!ProcessIdToSessionId(pid, &serverSession) ||
      !ProcessIdToSessionId(GetCurrentProcessId(), &clientSession) || serverSession != clientSession)
    return false;
  if (daemon)
    return sameExecutable(pid, QFileInfo(corePath).dir().filePath(QStringLiteral("deskflow-daemon.exe")));
  // System-owned service children cannot generally be opened by a standard
  // user. Their parent can still be checked against the SCM-registered daemon.
  if (registered && parentProcessId(pid) == registered) return true;
  return sameExecutable(pid, corePath);
}
}
#endif
