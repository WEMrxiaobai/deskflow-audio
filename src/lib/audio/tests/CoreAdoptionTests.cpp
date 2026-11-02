// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Duplicate-core recovery through isolated local IPC; no actual core or service is stopped.
#include "base/Log.h"
#include "common/Constants.h"
#include "common/CoreRuntimeIdentity.h"
#include "common/ExitCodes.h"
#include "common/Settings.h"
#include "common/VersionInfo.h"
#include "deskflow/ipc/IpcServer.h"
#include "gui/config/ServerConfig.h"
#include "gui/core/CoreProcess.h"
#include "gui/ipc/CoreIpcClient.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QSharedMemory>
#include <QTemporaryFile>
#include <QThread>
#include <QUuid>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using deskflow::core::ConnectionState;
using deskflow::core::ProcessState;
using deskflow::gui::CoreProcess;
std::atomic<bool> identityRejected{false};
void messages(QtMsgType type, const QMessageLogContext &, const QString &message)
{
  if (type == QtWarningMsg && message.contains(QStringLiteral("existing core has different settings")))
    identityRejected = true;
}
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()> &predicate, int timeout = 4000)
{
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout) {
    QApplication::processEvents();
    if (predicate())
      return true;
    QThread::msleep(2);
  }
  return predicate();
}
class TestCore : public deskflow::core::ipc::IpcServer
{
public:
  explicit TestCore(const QString &name) : IpcServer(nullptr, name, QStringLiteral("core"))
  {
  }
  int stops = 0;
  bool respondToStop = true;
  void dropConnections()
  {
    for (auto *socket : findChildren<QLocalSocket *>())
      socket->disconnectFromServer();
  }

private:
  void processCommand(QLocalSocket *client, const QString &command, const QStringList &) override
  {
    if (command == QStringLiteral("stop")) {
      ++stops;
      writeToClientSocket(client, QStringLiteral("ok"));
      if (respondToStop)
        broadcastCommand(QStringLiteral("bye"));
    }
  }
};
class TestDaemon : public deskflow::core::ipc::IpcServer
{
public:
  explicit TestDaemon(const QString &name) : IpcServer(nullptr, name, QStringLiteral("daemon"))
  {
  }
  int stops = 0;
  int starts = 0;
  std::function<void()> onStop;

private:
  void processCommand(QLocalSocket *client, const QString &command, const QStringList &) override
  {
    if (command == QStringLiteral("start")) {
      ++starts;
      writeToClientSocket(client, QStringLiteral("ok"));
    }
    if (command == QStringLiteral("stop")) {
      ++stops;
      writeToClientSocket(client, QStringLiteral("ok"));
      if (onStop)
        onStop();
    }
  }
};

void offlineQueueTest(const QString &name)
{
  TestCore server(name);
  server.listen();
  for (int i = 0; i < 1000; ++i) {
    server.broadcastCommand(QStringLiteral("connectionState"), QString::number(i));
    server.broadcastCommand(QStringLiteral("keyboardClientIdentities"), QString::number(i));
    server.broadcastCommand(QStringLiteral("unrecognisedClient"), QString::number(i));
  }
  server.broadcastCommand(QStringLiteral("retryIn"), QStringLiteral("5"));
  server.broadcastCommand(QStringLiteral("serverAddress"), QStringLiteral("192.0.2.1"));
  server.broadcastCommand(QStringLiteral("serverAddress"));
  server.broadcastCommand(QStringLiteral("unrecognisedClient"), QString(40000, QLatin1Char('x')));
  auto connectAndRead = [&](QLocalSocket &socket, QByteArray &received) {
    socket.connectToServer(name);
    check(waitFor([&] { return socket.state() == QLocalSocket::ConnectedState; }), "Fixture connection failed");
    socket.write(QStringLiteral("hello=%1+%2\n").arg(kVersion, kVersionGitSha).toUtf8());
    socket.flush();
    check(
        waitFor([&] {
          received += socket.readAll();
          return received.contains("connectionState=999\n") && received.contains("keyboardClientIdentities=999\n") &&
                 received.contains("serverAddress\n");
        }),
        "Current snapshots not delivered"
    );
  };
  QLocalSocket first;
  QByteArray firstMessages;
  connectAndRead(first, firstMessages);
  const auto lines = firstMessages.split('\n');
  int events = 0, states = 0, identities = 0;
  for (const auto &line : lines) {
    events += line.startsWith("unrecognisedClient=");
    states += line.startsWith("connectionState=");
    identities += line.startsWith("keyboardClientIdentities=");
  }
  check(
      events == 64 && firstMessages.contains("unrecognisedClient=936\n") &&
          firstMessages.contains("unrecognisedClient=999\n"),
      "Offline event count limit or eviction failed"
  );
  check(
      states == 1 && identities == 1 && !firstMessages.contains("retryIn=") &&
          !firstMessages.contains("serverAddress="),
      "Stale state replayed"
  );
  QLocalSocket second;
  QByteArray secondMessages;
  connectAndRead(second, secondMessages);
  check(!secondMessages.contains("unrecognisedClient="), "One-time events replayed to second GUI");
  server.broadcastCommand(QStringLiteral("serverAddress"), QStringLiteral("192.0.2.2"));
  QLocalSocket third;
  third.connectToServer(name);
  check(waitFor([&] { return third.state() == QLocalSocket::ConnectedState; }), "Third GUI connection failed");
  third.write(QStringLiteral("hello=%1+%2\n").arg(kVersion, kVersionGitSha).toUtf8());
  third.flush();
  QByteArray thirdMessages;
  check(
      waitFor([&] {
        thirdMessages += third.readAll();
        return thirdMessages.contains("serverAddress=192.0.2.2\n");
      }),
      "Updated server address was not replayed"
  );
}
} // namespace

int main(int argc, char **argv)
{
  // The parent starts this executable as its isolated fake core.
  if (argc > 1 && (std::strcmp(argv[1], "client") == 0 || std::strcmp(argv[1], "server") == 0))
    return s_exitDuplicate;
  QApplication::setApplicationName(kAppName);
  QApplication::setOrganizationName(QStringLiteral("DeskflowIntegrationTest"));
  QApplication app(argc, argv);
  Log coreLog;
  const auto folder = QApplication::applicationDirPath() + QStringLiteral("/settings");
  if (!QDir().mkpath(folder))
    return 1;
  QLockFile lock(folder + QStringLiteral("/audio-smoke.lock"));
  if (!lock.tryLock(10000))
    return 2;
  QFile config(folder + QStringLiteral("/") + kAppName + QStringLiteral(".conf"));
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return 3;
  const auto path = config.fileName();
  config.write("[gui]\nstartCoreWithGui=false\nenableUpdateCheck=false\n[core]\nprocessMode=1\n");
  config.close();
  int result = 0;
  try {
    check(Settings::settingsFile() == path, "Settings escaped isolated fixture");
    const auto name = QStringLiteral("deskflow-core-adoption-") + QUuid::createUuid().toString(QUuid::Id128);
    offlineQueueTest(name + QStringLiteral("-offline"));
    {
      const auto layoutPath = Settings::serverConfigFile();
      check(layoutPath.startsWith(folder + QLatin1Char('/')), "Server fixture layout escaped isolated settings");
      const QByteArray sentinel("existing server layout must not be overwritten\n");
      const auto writeSentinel = [&] {
        QFile layout(layoutPath);
        check(layout.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot create isolated layout fixture");
        check(layout.write(sentinel) == sentinel.size(), "Cannot write isolated layout fixture");
      };
      const auto readLayout = [&] {
        QFile layout(layoutPath);
        check(layout.open(QIODevice::ReadOnly), "Cannot read isolated layout fixture");
        return layout.readAll();
      };
      ServerConfig currentLayout;
      QTemporaryFile serialized;
      check(serialized.open(), "Cannot serialize server layout fixture");
      currentLayout.save(serialized);
      serialized.flush();
      check(serialized.seek(0), "Cannot rewind server layout fixture");
      const auto layoutBytes = serialized.readAll();
      for (const bool matching : {true, false}) {
        writeSentinel();
        const auto suffix = matching ? QStringLiteral("-server-match") : QStringLiteral("-server-mismatch");
        QSharedMemory singleton(name + suffix + QStringLiteral("-singleton"));
        check(singleton.create(1), "Cannot reserve isolated singleton fixture");
        TestCore existing(name + suffix);
        existing.listen();
        auto identity = coreRuntimeIdentity(true);
        setCoreRuntimeLayout(identity, matching ? layoutBytes : QByteArray("different layout"));
        existing.broadcastCommand(QStringLiteral("coreIdentity"),
                                  QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toBase64()));
        existing.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Listening"));
        CoreProcess probe(currentLayout, name + suffix, QApplication::applicationFilePath(),
                          name + suffix + QStringLiteral("-daemon"), name + suffix + QStringLiteral("-singleton"));
        probe.setMode(Settings::CoreMode::Server);
        identityRejected = false;
        const auto oldHandler = qInstallMessageHandler(messages);
        probe.start(Settings::ProcessMode::Desktop);
        const bool decided = waitFor([&] {
          return matching ? (probe.isStarted() && probe.connectionState() == ConnectionState::Listening)
                          : identityRejected.load();
        });
        qInstallMessageHandler(oldHandler);
        check(decided && (matching || !probe.isStarted()), "Existing server layout comparison failed");
        check(existing.stops == 0 && readLayout() == sentinel, "Adoption modified the running server layout");
      }
      writeSentinel();
      ServerConfig newLayout;
      CoreProcess fresh(newLayout, name + QStringLiteral("-fresh-core"), QApplication::applicationFilePath(),
                        name + QStringLiteral("-fresh-daemon"), name + QStringLiteral("-fresh-singleton"));
      fresh.setMode(Settings::CoreMode::Server);
      fresh.start(Settings::ProcessMode::Desktop);
      check(readLayout() == layoutBytes, "Fresh singleton did not persist the selected layout");
      fresh.stop(Settings::ProcessMode::Desktop);
      QFile::remove(layoutPath);
    }
    {
      TestDaemon later(name + QStringLiteral("-cancel-daemon"));
      ServerConfig cancelConfig;
      CoreProcess cancelled(cancelConfig, name + QStringLiteral("-cancel-core"), QApplication::applicationFilePath(),
                            name + QStringLiteral("-cancel-daemon"));
      cancelled.setMode(Settings::CoreMode::Client);
      cancelled.start(Settings::ProcessMode::Service);
      check(cancelled.processState() == ProcessState::Starting, "Service did not wait for daemon");
      cancelled.stop(Settings::ProcessMode::Service);
      check(cancelled.processState() == ProcessState::Stopped, "Cancelling pending start did not stop");
      later.listen();
      QElapsedTimer timer;
      timer.start();
      while (timer.elapsed() < 1800) {
        QApplication::processEvents();
        QThread::msleep(2);
      }
      check(later.starts == 0 && cancelled.processState() == ProcessState::Stopped,
            "Cancelled start was sent when daemon later connected");
    }
    {
      TestCore other(name + QStringLiteral("-mismatch"));
      other.listen();
      auto wrong = coreRuntimeIdentity(false);
      wrong["target"] = QStringLiteral("different-server.example");
      other.broadcastCommand(
          QStringLiteral("coreIdentity"),
          QString::fromLatin1(QJsonDocument(wrong).toJson(QJsonDocument::Compact).toBase64())
      );
      other.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Connected"));
      ServerConfig otherConfig;
      CoreProcess rejected(
          otherConfig, name + QStringLiteral("-mismatch"), QApplication::applicationFilePath(),
          name + QStringLiteral("-unreachable-daemon")
      );
      rejected.setMode(Settings::CoreMode::Client);
      identityRejected = false;
      const auto oldHandler = qInstallMessageHandler(messages);
      rejected.start(Settings::ProcessMode::Desktop);
      const bool rejectedIdentity = waitFor([] { return identityRejected.load(); });
      qInstallMessageHandler(oldHandler);
      check(
          rejectedIdentity && !rejected.isStarted() && other.stops == 0,
          "A different target was adopted or its running core was stopped"
      );
    }
    {
      TestCore legacy(name + QStringLiteral("-legacy"));
      TestDaemon daemon(name + QStringLiteral("-legacy-daemon"));
      legacy.listen();
      daemon.listen();
      legacy.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Connected"));
      daemon.onStop = [&] { legacy.dropConnections(); };
      ServerConfig legacyConfig;
      CoreProcess migrating(
          legacyConfig, name + QStringLiteral("-legacy"), QApplication::applicationFilePath(),
          name + QStringLiteral("-legacy-daemon")
      );
      migrating.setMode(Settings::CoreMode::Client);
      migrating.start(Settings::ProcessMode::Desktop);
      check(
          waitFor([&] { return daemon.stops == 1 && migrating.processState() == ProcessState::RetryPending; }, 6000),
          "Legacy core did not clear its watchdog and retry after daemon-driven shutdown"
      );
    }
    {
      TestCore replacement(name + QStringLiteral("-replacement"));
      replacement.listen();
      auto identity = coreRuntimeIdentity(false);
      replacement.broadcastCommand(QStringLiteral("coreIdentity"),
          QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toBase64()));
      replacement.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Connected"));
      ServerConfig replacementConfig;
      CoreProcess probe(replacementConfig, name + QStringLiteral("-replacement"), QApplication::applicationFilePath(),
                        name + QStringLiteral("-replacement-daemon"));
      probe.setMode(Settings::CoreMode::Client);
      probe.start(Settings::ProcessMode::Desktop);
      check(waitFor([&] { return probe.isStarted() && probe.connectionState() == ConnectionState::Connected; }),
            "Replacement fixture failed initial adoption");
      identity["target"] = QStringLiteral("different-after-reconnect.example");
      replacement.broadcastCommand(QStringLiteral("coreIdentity"),
          QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toBase64()));
      identityRejected = false;
      const auto oldHandler = qInstallMessageHandler(messages);
      replacement.dropConnections();
      const bool rejectedReplacement = waitFor([&] { return identityRejected.load(); }, 6000);
      qInstallMessageHandler(oldHandler);
      if (!rejectedReplacement || probe.isStarted() || replacement.stops != 0)
        std::cerr << "replacement: rejected=" << rejectedReplacement << " started=" << probe.isStarted()
                  << " stops=" << replacement.stops << '\n';
      check(rejectedReplacement && !probe.isStarted() && replacement.stops == 0,
            "Changed core identity was adopted or stopped after reconnect");
    }
    TestCore server(name);
    TestDaemon daemon(name + QStringLiteral("-daemon"));
    server.listen();
    daemon.listen();
    deskflow::gui::ipc::CoreIpcClient existingGui(nullptr, name);
    bool previousConnected = false;
    QObject::connect(&existingGui, &deskflow::gui::ipc::CoreIpcClient::connected, [&] { previousConnected = true; });
    existingGui.connectToServer();
    check(waitFor([&] { return previousConnected; }), "Existing GUI did not connect to isolated core");
    // A connected GUI consumes broadcasts, so there are no pending messages for
    // a newly attached GUI. The new one must still receive the latest snapshot.
    server.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Connecting"));
    server.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Connected"));
    server.broadcastCommand(QStringLiteral("connectedClients"), QStringLiteral("alpha,beta"));
    server.broadcastCommand(
        QStringLiteral("coreIdentity"),
        QString::fromLatin1(QJsonDocument(coreRuntimeIdentity(false)).toJson(QJsonDocument::Compact).toBase64())
    );
    ServerConfig serverConfig;
    CoreProcess recovered(serverConfig, name, QApplication::applicationFilePath(), name + QStringLiteral("-daemon"));
    recovered.setMode(Settings::CoreMode::Client);
    QStringList clients;
    QObject::connect(&recovered, &CoreProcess::connectedClientsChanged, [&](const auto &value) { clients = value; });
    recovered.start(Settings::ProcessMode::Desktop);
    check(
        waitFor([&] {
          return recovered.processState() == ProcessState::Started &&
                 recovered.connectionState() == ConnectionState::Connected &&
                 clients == QStringList({QStringLiteral("alpha"), QStringLiteral("beta")});
        }),
        "Duplicate recovery did not adopt core and restore current status"
    );
    QElapsedTimer delayedAttach;
    delayedAttach.start();
    while (delayedAttach.elapsed() < 1300) {
      QApplication::processEvents();
      QThread::msleep(2);
    }
    check(
        recovered.isStarted() && recovered.connectionState() == ConnectionState::Connected,
        "Delayed normal startup replaced or lost the adopted connection"
    );
    server.broadcastCommand(QStringLiteral("connectionState"), QStringLiteral("Listening"));
    server.broadcastCommand(QStringLiteral("connectedClients"));
    check(
        waitFor([&] { return recovered.connectionState() == ConnectionState::Listening && clients.isEmpty(); }),
        "Adopted core did not deliver later status changes"
    );
    server.dropConnections();
    check(waitFor([&] { return recovered.connectionState() == ConnectionState::Disconnected; }),
          "IPC disconnect did not clear the adopted core status");
    check(waitFor([&] { return recovered.isStarted() && recovered.connectionState() == ConnectionState::Listening; }, 6000),
          "Adopted core did not reconnect and restore its current status");
    int startingTransitions = 0;
    QObject::connect(&recovered, &CoreProcess::processStateChanged, [&](ProcessState state) {
      if (state == ProcessState::Starting)
        ++startingTransitions;
    });
    server.respondToStop = false;
    recovered.restart();
    check(waitFor([&] { return server.stops == 1; }), "Restart did not request old core stop");
    QElapsedTimer waitForOldCore;
    waitForOldCore.start();
    while (waitForOldCore.elapsed() < 700) {
      QApplication::processEvents();
      QThread::msleep(2);
    }
    check(startingTransitions == 0, "Restart launched a new core before old core confirmed exit");
    server.broadcastCommand(QStringLiteral("bye"));
    check(waitFor([&] { return startingTransitions > 0 && recovered.isStarted() &&
                               recovered.connectionState() == ConnectionState::Listening; }, 6000),
          "Restart did not resume after old core confirmed exit");
    server.respondToStop = true;
    recovered.stop(Settings::ProcessMode::Desktop);
    check(
        waitFor([&] {
          return daemon.stops == 2 && server.stops == 2 && recovered.processState() == ProcessState::Stopped;
        }),
        "Stopping adopted core did not use its IPC connection"
    );
    check(recovered.connectionState() == ConnectionState::Disconnected, "Stopped core still looks connected");
    std::cout << "adoption_and_layout_protection=pass legacy_migration=pass cancelled_start=pass "
                 "ipc_reconnect=pass restart_wait=pass stop_daemon_and_core=pass\n";
  } catch (const std::exception &error) {
    std::cerr << "Core adoption test failed: " << error.what() << '\n';
    result = 4;
  }
  Settings::save(false);
  QFile::remove(path);
  return result;
}
