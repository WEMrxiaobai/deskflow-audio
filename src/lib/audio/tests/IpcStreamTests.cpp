// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "base/Log.h"
#include "common/VersionInfo.h"
#include "deskflow/ipc/IpcServer.h"
#include "deskflow/ipc/WindowsIpcAccess.h"
#include "gui/ipc/IpcClient.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QLocalServer>
#include <QProcess>
#include <QThread>
#include <QUuid>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()> &predicate)
{
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 4000) {
    QCoreApplication::processEvents();
    if (predicate())
      return true;
    QThread::msleep(2);
  }
  return predicate();
}
class Server : public deskflow::core::ipc::IpcServer
{
public:
  explicit Server(const QString &name, const QString &type = QStringLiteral("core")) : IpcServer(nullptr, name, type) {}
  QStringList commands;
private:
  void processCommand(QLocalSocket *client, const QString &command, const QStringList &parts) override
  {
    commands.append(command + QStringLiteral(":") + parts.mid(1).join('='));
    if (command == QStringLiteral("child")) {
      client->write("ok\n");
      client->flush();
    }
  }
};
class Gui : public deskflow::gui::ipc::IpcClient
{
public:
  explicit Gui(const QString &name) : IpcClient(nullptr, name, QStringLiteral("core")) {}
  void stop() { sendMessage(QStringLiteral("stop")); }
};
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  Log log;
  if (app.arguments().size() == 3 && app.arguments()[1] == QStringLiteral("--peer")) {
    QLocalSocket peer;
    peer.connectToServer(app.arguments()[2]);
    if (!peer.waitForConnected(4000))
      return 2;
    peer.write("child=separate-process\n");
    if (!peer.waitForBytesWritten(4000))
      return 3;
    return peer.waitForReadyRead(4000) && peer.readAll().contains("ok\n") ? 0 : 4;
  }
  try {
#ifdef Q_OS_WIN
    using deskflow::core::ipc::detail::AccessServer;
    using deskflow::core::ipc::detail::allowSession;
    check(allowSession(AccessServer::Core, 4, 4, false, false), "same-session core denied");
    check(!allowSession(AccessServer::Core, 4, 5, true, false), "cross-session core allowed");
    check(allowSession(AccessServer::Daemon, 0, 4, true, false), "active console/RDP denied");
    check(!allowSession(AccessServer::Daemon, 0, 4, false, false), "inactive console/RDP allowed");
    check(allowSession(AccessServer::Daemon, 0, 0, false, true), "LocalSystem denied");
    check(!allowSession(AccessServer::Daemon, 0, 0, false, false), "non-system session zero allowed");
#endif
    const auto name = QStringLiteral("deskflow-ipc-stream-") + QUuid::createUuid().toString(QUuid::Id128);
#ifdef Q_OS_WIN
    DWORD ownSession = 0, stateBytes = 0;
    LPWSTR stateInfo = nullptr;
    const bool haveState = ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) &&
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, ownSession, WTSConnectState, &stateInfo, &stateBytes);
    const bool activeSession = haveState && stateInfo && stateBytes >= sizeof(WTS_CONNECTSTATE_CLASS) &&
        *reinterpret_cast<WTS_CONNECTSTATE_CLASS *>(stateInfo) == WTSActive;
    if (stateInfo)
      WTSFreeMemory(stateInfo);
    if (ownSession != 0 && activeSession) {
      Server daemon(name + QStringLiteral("-daemon"), QStringLiteral("daemon"));
      daemon.listen();
      QLocalSocket daemonClient;
      daemonClient.connectToServer(name + QStringLiteral("-daemon"));
      check(waitFor([&] { return daemonClient.state() == QLocalSocket::ConnectedState; }), "daemon connect failed");
      daemonClient.write("ping=active\n");
      daemonClient.flush();
      check(waitFor([&] { return daemon.commands.contains(QStringLiteral("ping:active")); }),
            "real active Windows session was rejected");
    } else {
      std::cout << "active-session daemon integration skipped (no active interactive session)\n";
    }
#endif
    Server server(name);
    server.listen();
    QProcess peer;
    peer.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--peer"), name});
    check(waitFor([&] { return server.commands.contains(QStringLiteral("child:separate-process")); }),
          "same-session separate process was rejected");
    check(waitFor([&] { return peer.state() == QProcess::NotRunning; }) && peer.exitCode() == 0,
          "same-session peer failed");
    server.commands.clear();
    QLocalSocket client, other;
    client.connectToServer(name);
    other.connectToServer(name);
    check(waitFor([&] { return client.state() == QLocalSocket::ConnectedState &&
                                other.state() == QLocalSocket::ConnectedState; }), "connect failed");
    client.write("one=par");
    client.flush();
    other.write("other=complete\n");
    other.flush();
    check(waitFor([&] { return server.commands.size() == 1; }), "independent client was blocked");
    check(server.commands == QStringList{QStringLiteral("other:complete")}, "partial line was processed");
    client.write("tial\ntwo=whole\nthree=ta");
    client.flush();
    check(waitFor([&] { return server.commands.size() == 3; }), "merged lines were not processed");
    check(server.commands[1] == QStringLiteral("one:partial") && server.commands[2] == QStringLiteral("two:whole"),
          "fragment or merged line was corrupted");
    client.write("il\n");
    client.flush();
    check(waitFor([&] { return server.commands.size() == 4; }) && server.commands[3] == QStringLiteral("three:tail"),
          "partial tail was lost");
    client.write(QByteArray(65536, 'x'));
    client.flush();
    check(waitFor([&] { return client.state() == QLocalSocket::UnconnectedState; }), "oversized input not closed");
    other.write("still=alive\n");
    other.flush();
    check(waitFor([&] { return server.commands.size() == 5; }) && server.commands[4] == QStringLiteral("still:alive"),
          "oversized peer affected another connection");
    const auto queueName = name + QStringLiteral("-queue");
    Server queued(queueName);
    queued.listen();
    for (int i = 0; i < 6; ++i)
      queued.broadcastCommand(QStringLiteral("event"), QString::number(i) + QString(6000, QLatin1Char('x')));
    queued.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Connected"));
    queued.broadcastCommand(QStringLiteral("keyboardServerFingerprint"), QStringLiteral("stale"));
    queued.broadcastCommand(QStringLiteral("keyboardServerFingerprint"));
    QLocalSocket receiver;
    receiver.connectToServer(queueName);
    check(waitFor([&] { return receiver.state() == QLocalSocket::ConnectedState; }), "queue connect failed");
    receiver.write(QStringLiteral("hello=%1+%2\n").arg(kVersion, kVersionGitSha).toUtf8());
    receiver.flush();
    QByteArray received;
    check(waitFor([&] {
      received += receiver.readAll();
      return received.contains("connectionState=Connected\n") && received.contains("keyboardServerFingerprint\n");
    }), "queued messages not received");
    check(!received.contains("event=0") && received.contains("event=1") && received.contains("event=5") &&
              received.count("event=") == 5, "queued byte budget did not evict oldest event");
    check(received.contains("keyboardServerFingerprint\n") && !received.contains("keyboardServerFingerprint="),
          "stale keyboard fingerprint was replayed");
    QLocalServer impostor;
    const auto fakeName = name + QStringLiteral("-fake");
    check(impostor.listen(fakeName), "fake server listen failed");
    Gui gui(fakeName);
    bool failed = false;
    QObject::connect(&gui, &Gui::connectionFailed, [&] { failed = true; });
    gui.connectToServer();
    check(waitFor([&] { return impostor.hasPendingConnections(); }), "fake server connection failed");
    auto *fake = impostor.nextPendingConnection();
    fake->write("hello=wrong-version\n");
    fake->flush();
    check(waitFor([&] { return failed; }) && !gui.isConnected(), "invalid hello was accepted");
    QLocalServer oversizedServer;
    const auto oversizedName = name + QStringLiteral("-oversized");
    check(oversizedServer.listen(oversizedName), "oversized server listen failed");
    Gui boundedGui(oversizedName);
    bool oversizedFailed = false;
    QObject::connect(&boundedGui, &Gui::connectionFailed, [&] { oversizedFailed = true; });
    boundedGui.connectToServer();
    check(waitFor([&] { return oversizedServer.hasPendingConnections(); }), "oversized server connection failed");
    auto *oversized = oversizedServer.nextPendingConnection();
    oversized->write(QByteArray(65536, 'x'));
    oversized->flush();
    check(waitFor([&] { return oversizedFailed; }) && !boundedGui.isConnected(), "oversized server input accepted");
    QLocalServer oldServer;
    const auto oldName = name + QStringLiteral("-old");
    check(oldServer.listen(oldName), "old server listen failed");
    Gui migratingGui(oldName);
    bool mismatch = false;
    QObject::connect(&migratingGui, &Gui::versionMismatch, [&] { mismatch = true; });
    migratingGui.connectToServer();
    check(waitFor([&] { return oldServer.hasPendingConnections(); }), "old server connection failed");
    auto *old = oldServer.nextPendingConnection();
    old->write("versionMismatch=old-version\n");
    old->flush();
    check(waitFor([&] { return mismatch; }) && migratingGui.isConnected(), "version mismatch no longer migratable");
    migratingGui.stop();
    QByteArray oldReceived;
    check(waitFor([&] {
      oldReceived += old->readAll();
      return oldReceived.contains("stop\n");
    }), "version mismatch stop was not sent");
    std::cout << "ipc fragmentation, coalescing, limits and isolation: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "IPC stream test failed: " << error.what() << '\n';
    return 1;
  }
}
