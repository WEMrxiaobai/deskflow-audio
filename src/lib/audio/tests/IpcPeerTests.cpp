// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/ipc/WindowsIpcPeer.h"
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>
#include <iostream>

#ifdef Q_OS_WIN
using namespace deskflow::gui::ipc::detail;
struct IsolatedWindowsOperations : WindowsPeerOperations {
  DWORD daemonPid(LookupDiagnostic &) const { return 0; }
};
struct FakeOperations {
  ULONG pid = 42;
  DWORD registered = 0;
  DWORD parent = 0;
  DWORD serverSession = 2;
  DWORD clientSession = 2;
  bool pidOk = true;
  bool serverSessionOk = true;
  bool clientSessionOk = true;
  DWORD lastError = ERROR_ACCESS_DENIED;
  PeerVerification image{};
  LookupDiagnostic daemonDiagnostic{};
  LookupDiagnostic parentDiagnostic{};
  mutable QString expectedImage;
  bool serverPid(ULONG &out) const { out = pid; return pidOk; }
  DWORD daemonPid(LookupDiagnostic &out) const { out = daemonDiagnostic; return registered; }
  DWORD parentPid(DWORD, LookupDiagnostic &out) const { out = parentDiagnostic; return parent; }
  DWORD currentPid() const { return 99; }
  bool session(DWORD id, DWORD &out) const {
    if (id == currentPid()) { out = clientSession; return clientSessionOk; }
    out = serverSession; return serverSessionOk;
  }
  DWORD error() const { return lastError; }
  PeerVerification executable(DWORD, const QString &expected) const { expectedImage = expected; return image; }
};
#endif

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
  const QString core = QStringLiteral("C:/deskflow/deskflow-core.exe");
  int failures = 0;
  auto check = [&](bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
  };
  FakeOperations ops;
  auto verify = [&](bool daemon = false) { return verifyPipeServerWith(ops, daemon, core); };
  ops.pidOk = false;
  auto result = verify();
  check(result.failure == PeerFailure::PipeServerPid && result.error == ERROR_ACCESS_DENIED, "pipe PID error");
  ops.pidOk = true; ops.pid = 0;
  check(verify().failure == PeerFailure::PipeServerPid, "zero pipe PID");
  ops.pid = 42; ops.serverSessionOk = false;
  check(verify().failure == PeerFailure::ServerSession && verify().error == ERROR_ACCESS_DENIED, "server session error");
  ops.serverSessionOk = true; ops.clientSessionOk = false;
  check(verify().failure == PeerFailure::ClientSession && verify().error == ERROR_ACCESS_DENIED, "client session error");
  ops.clientSessionOk = true; ops.clientSession = 3;
  check(verify().failure == PeerFailure::SessionMismatch, "session mismatch");
  ops.registered = ops.pid;
  check(verify(true).verified(), "registered daemon should be trusted");
  check(!verify(false).verified(), "registered PID does not bypass core session check");
  ops.clientSession = 2; ops.registered = 7; ops.parent = 7;
  check(verify().verified(), "registered daemon child");
  ops.parent = 0;
  for (auto failure : {PeerFailure::OpenServerProcess, PeerFailure::QueryServerImage,
                       PeerFailure::ResolveServerImage, PeerFailure::ResolveExpectedImage,
                       PeerFailure::ExecutableMismatch}) {
    ops.image = {failure, ERROR_ACCESS_DENIED};
    result = verify();
    check(result.failure == failure && result.error == ERROR_ACCESS_DENIED, "image rejection propagated");
  }
  ops.daemonDiagnostic = {"service query", ERROR_ACCESS_DENIED};
  ops.parentDiagnostic = {"parent query", ERROR_ACCESS_DENIED};
  result = verify();
  check(result.daemonLookup.error == ERROR_ACCESS_DENIED && result.parentLookup.error == ERROR_ACCESS_DENIED,
        "auxiliary lookup failures propagated");
  ops.image = {};
  check(verify().verified() && ops.expectedImage == core, "core executable fallback");
  check(verify(true).verified() && ops.expectedImage.endsWith(QStringLiteral("/deskflow-daemon.exe")),
        "daemon executable fallback");
  QLocalServer server;
  const auto name = QStringLiteral("deskflow-peer-test-") + QUuid::createUuid().toString(QUuid::Id128);
  check(server.listen(name), "isolated pipe listen");
  QLocalSocket client;
  client.connectToServer(name);
  const bool connected = client.waitForConnected(2000) && server.waitForNewConnection(2000);
  check(connected, "isolated pipe connect");
  if (connected) {
    const IsolatedWindowsOperations real{{reinterpret_cast<HANDLE>(client.socketDescriptor())}};
    check(verifyPipeServerWith(real, false, app.applicationFilePath()).verified(), "real PID and executable match");
    check(!verifyPipeServerWith(real, false, app.applicationDirPath() + QStringLiteral("/wrong-program.exe")).verified(),
          "real executable mismatch rejected");
    check(!verifyPipeServerWith(real, true, app.applicationFilePath()).verified(), "real daemon mismatch rejected");
  }
  if (failures) return 1;
  std::cout << "IPC peer verification policy and isolated Windows pipe: PASS\n";
#endif
  return 0;
}
