/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreProcess.h"

#include "common/Constants.h"
#include "common/CoreRuntimeIdentity.h"
#include "common/ExitCodes.h"
#include "common/VersionInfo.h"
#include "gui/ipc/CoreIpcClient.h"
#include "gui/ipc/DaemonIpcClient.h"

#if defined(Q_OS_MACOS)
#include "OSXHelpers.h"
#endif

#ifdef Q_OS_LINUX
#include <signal.h>
#include <sys/prctl.h>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMetaEnum>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSharedMemory>
#include <QTemporaryFile>
#include <utility>

namespace deskflow::gui {

const int kRetryDelay = 1000;
const auto kLineSplitRegex = QRegularExpression("\r|\n|\r\n");

QString CoreProcess::processModeToString(const Settings::ProcessMode mode)
{
  return QVariant::fromValue(mode).toString().toLower();
}

QString CoreProcess::processStateToString(const CoreProcess::ProcessState state)
{
  return QVariant::fromValue(state).toString().toLower();
}

/**
 * @brief Wraps options that contain spaces in quotes
 *
 * Useful to handle things like paths with spaces (e.g. "C:\Program Files").
 *
 * Can also be used to create a representation of a command that can be pasted
 * into a terminal.
 */
QString CoreProcess::makeQuotedArgs(const QString &app, const QStringList &args)
{
  QStringList command = {app};
  command.append(args);

  static const auto quote = QStringLiteral("\"");
  static const auto space = QStringLiteral(" ");
  QStringList quoted;
  for (const auto &item : std::as_const(command)) {
    auto temp = item.simplified();
    if (const auto wrapped = (temp.startsWith(quote) && temp.endsWith(quote)); temp.contains(space) && !wrapped) {
      temp = QStringLiteral("%1%2%1").arg(quote, temp);
    }
    quoted.append(temp);
  }

  return quoted.join(space);
}

/**
 * @brief If IPv6, ensures the IP is surround in square brackets.
 */
QString CoreProcess::wrapIpv6(const QString &address)
{
  static const auto colon = QStringLiteral(":");
  static const auto openBracket = QStringLiteral("[");
  static const auto closeBracket = QStringLiteral("]");

  if (!address.contains(colon) || address.isEmpty()) {
    return address;
  }

  QString wrapped = address;

  if (!address.startsWith(openBracket)) {
    wrapped.prepend(openBracket);
  }

  if (!address.endsWith(closeBracket)) {
    wrapped.append(closeBracket);
  }

  return wrapped;
}

//
// CoreProcess
//

CoreProcess::CoreProcess(
    const ServerConfig &serverConfig, const QString &coreIpcName, const QString &coreExecutable, const QString &daemonIpcName,
    const QString &singletonName
)
    : m_serverConfig(serverConfig),
      m_daemonIpcClient{new ipc::DaemonIpcClient(this, daemonIpcName)},
      m_coreIpcName(coreIpcName.isEmpty() ? kCoreIpcName : coreIpcName),
      m_daemonIpcName(daemonIpcName.isEmpty() ? kDaemonIpcName : daemonIpcName),
      m_singletonName(singletonName.isEmpty() ? kCoreBinName : singletonName)
{
  m_appPath = coreExecutable.isEmpty()
                  ? QStringLiteral("%1/%2").arg(QCoreApplication::applicationDirPath(), kCoreBinName)
                  : coreExecutable;
  if (!QFile::exists(m_appPath)) {
    qCritical("core server binary does not exist");
  }

  connect(m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, &CoreProcess::daemonIpcClientConnected);
  connect(
      m_daemonIpcClient, &ipc::DaemonIpcClient::connectionFailed, this, &CoreProcess::daemonIpcClientConnectionFailed
  );
  connect(m_daemonIpcClient, &ipc::DaemonIpcClient::logPathReceived, this, &CoreProcess::setupDaemonLogTail);

  connect(&m_retryTimer, &QTimer::timeout, this, [this] {
    if (m_processState == ProcessState::RetryPending) {
      start();
    } else {
      qDebug("retry cancelled, process state is not retry pending");
    }
  });
}

void CoreProcess::onProcessReadyReadStandardOutput()
{
  if (m_process) {
    handleLogLines(m_process->readAllStandardOutput());
  }
}

void CoreProcess::onProcessReadyReadStandardError()
{
  if (m_process) {
    handleLogLines(m_process->readAllStandardError());
  }
}

void CoreProcess::daemonIpcClientConnected()
{
  applyLogLevel();
  m_daemonIpcClient->requestLogPath();
}

void CoreProcess::checkExistingProcess()
{
  qInfo("checking existing core");

  auto *client = new ipc::CoreIpcClient(this, m_coreIpcName);
  struct AdoptionState
  {
    bool decided = false;
    bool adopted = false;
    bool migrating = false;
    std::uint64_t connectionEpoch = 0;
    QMap<QString, QString> pending;
  };
  auto adoption = std::make_shared<AdoptionState>();
  const auto generation = m_operationGeneration;
  connect(client, &ipc::CoreIpcClient::commandReceived, this, [this, client, adoption, generation](const QString &command, const QString &args) {
    if (!adoption->adopted && generation != m_operationGeneration) {
      client->disconnectFromServer();
      client->deleteLater();
      return;
    }
    if (command == QStringLiteral("coreIdentity") && !adoption->decided) {
      adoption->decided = true;
      const auto identity = QJsonDocument::fromJson(QByteArray::fromBase64(args.toLatin1()));
      const bool server = m_mode == Settings::CoreMode::Server;
      auto expected = coreRuntimeIdentity(server);
      if (server && !Settings::value(Settings::Server::ExternalConfig).toBool()) {
        // Compare the current GUI layout, without modifying the file read by
        // the existing server. ServerConfig writes to QFile, so use a private
        // temporary file to produce exactly the same bytes as persistence.
        QTemporaryFile snapshot;
        if (!snapshot.open()) {
          expected["configValid"] = false;
        } else {
          m_serverConfig.save(snapshot);
          if (!snapshot.flush() || !snapshot.seek(0))
            expected["configValid"] = false;
          else
            setCoreRuntimeLayout(expected, snapshot.readAll());
        }
      }
      if (!identity.isObject() || identity.object() != expected ||
          (server && !expected.value("configValid").toBool())) {
        qWarning("existing core has different settings; stop it before connecting with the current configuration");
        if (m_coreIpcClient == client) {
          m_coreIpcClient = nullptr;
          m_adoptedExistingCore = false;
          clearCoreStatus();
        }
        setProcessState(ProcessState::Stopped);
        setConnectionState(ConnectionState::Disconnected);
        client->disconnectFromServer();
        client->deleteLater();
        return;
      }
      adoption->adopted = true;
      if (m_coreIpcClient && m_coreIpcClient != client) {
        m_coreIpcClient->disconnectFromServer();
        m_coreIpcClient->deleteLater();
      }
      m_coreIpcClient = client;
      m_adoptedExistingCore = true;
      setProcessState(ProcessState::Started);
      for (auto it = adoption->pending.cbegin(); it != adoption->pending.cend(); ++it) {
        const auto command = it.key(), args = it.value();
        const auto epoch = adoption->connectionEpoch;
        QMetaObject::invokeMethod(
            this, [this, client, adoption, generation, epoch, command, args] {
              if (generation == m_operationGeneration && m_coreIpcClient == client && adoption->adopted &&
                  epoch == adoption->connectionEpoch)
                onCoreIpcMessageReceived(command, args);
            }, Qt::QueuedConnection
        );
      }
      adoption->pending.clear();
      qInfo("existing core matches the current settings and is now managed by the GUI");
    } else if (command != QStringLiteral("coreIdentity")) {
      if (adoption->adopted) {
        const auto epoch = adoption->connectionEpoch;
        QMetaObject::invokeMethod(
            this, [this, client, adoption, generation, epoch, command, args] {
              if (generation == m_operationGeneration && m_coreIpcClient == client && adoption->adopted &&
                  epoch == adoption->connectionEpoch)
                onCoreIpcMessageReceived(command, args);
            }, Qt::QueuedConnection
        );
      } else if (!adoption->decided &&
               (command == QStringLiteral("connectionState") || command == QStringLiteral("connectedClients") ||
                command == QStringLiteral("secureSocket") || command == QStringLiteral("keyboardClientIdentities") ||
                command == QStringLiteral("keyboardServerFingerprint") || command == QStringLiteral("serverAddress")))
        adoption->pending.insert(command, args);
    }
  });
  connect(client, &ipc::CoreIpcClient::connected, this, [this, client] {
    qInfo("existing core has matching version; checking its running configuration");
  });
  // An older core cannot prove which settings it is using. Replace it instead
  // of adopting it. Clear an active daemon watchdog first so it cannot revive
  // the old process while the GUI starts the configured replacement.
  const auto migrate = [this, client, adoption, generation] {
    if (generation != m_operationGeneration)
      return;
    adoption->migrating = true;
    stopExistingCoreSafely(client);
  };
  connect(client, &ipc::CoreIpcClient::connected, this, [this, client, adoption, migrate, generation] {
    const auto epoch = ++adoption->connectionEpoch;
    QTimer::singleShot(2000, client, [this, client, adoption, migrate, generation, epoch] {
      if (generation != m_operationGeneration || epoch != adoption->connectionEpoch || adoption->decided)
        return;
      adoption->decided = true;
      if (m_coreIpcClient == client) {
        // A replacement pipe must prove its identity; do not stop an unknown
        // process just because it did not send an identity snapshot.
        qWarning("reconnected core did not provide its configuration");
        m_coreIpcClient = nullptr;
        m_adoptedExistingCore = false;
        clearCoreStatus();
        setProcessState(ProcessState::Stopped);
        client->disconnectFromServer();
        client->deleteLater();
      } else {
        qWarning("existing core has no configuration snapshot; replacing it with the current configuration");
        migrate();
      }
    });
  });
  connect(client, &ipc::CoreIpcClient::versionMismatch, this, [this, client, migrate, generation] {
    if (generation != m_operationGeneration)
      return;
    if (m_coreIpcClient == client) {
      qWarning("reconnected core has a different version");
      m_coreIpcClient = nullptr;
      m_adoptedExistingCore = false;
      clearCoreStatus();
      setProcessState(ProcessState::Stopped);
      client->disconnectFromServer();
      client->deleteLater();
    } else {
      qInfo("existing core has mismatched version, replacing it safely");
      migrate();
    }
  });
  connect(client, &ipc::CoreIpcClient::serverShutdown, this, [this, client, adoption, generation] {
    qInfo("existing core stopped successfully");
    client->deleteLater();
    if (m_coreIpcClient == client) {
      m_coreIpcClient = nullptr;
      m_adoptedExistingCore = false;
      clearCoreStatus();
      setProcessState(ProcessState::Stopped);
      return;
    }
    if (generation != m_operationGeneration && !adoption->adopted)
      return;
    setProcessState(ProcessState::RetryPending);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.start(kRetryDelay);
  });
  connect(client, &ipc::CoreIpcClient::connectionFailed, this, [this, client, adoption, generation] {
    qCritical("could not contact existing core");
    if (m_coreIpcClient == client) {
      // The core may still be running after a pipe interruption. Clear all
      // leases immediately, then reconnect the same adopted client and replay
      // current snapshots; never start a competing core merely for IPC loss.
      clearCoreStatus();
      adoption->adopted = false;
      adoption->decided = false;
      adoption->pending.clear();
      ++adoption->connectionEpoch;
      QTimer::singleShot(kRetryDelay, this, [this, client, generation] {
        if (generation == m_operationGeneration && m_coreIpcClient == client &&
            m_processState == ProcessState::Started)
          client->connectToServer();
      });
      return;
    } else if (adoption->migrating && generation == m_operationGeneration) {
      setProcessState(ProcessState::RetryPending);
      m_retryTimer.setSingleShot(true);
      m_retryTimer.start(kRetryDelay);
    } else if (generation == m_operationGeneration) {
      setProcessState(ProcessState::Stopped);
      setConnectionState(ConnectionState::Disconnected);
    }
    client->deleteLater();
  });
  client->connectToServer();
}

void CoreProcess::stopExistingCoreSafely(ipc::CoreIpcClient *client)
{
  auto done = std::make_shared<bool>(false);
  const auto generation = m_operationGeneration;
  auto stopOld = [this, client, done, generation] {
    if (generation == m_operationGeneration && !std::exchange(*done, true))
      client->sendStop();
  };
  auto *daemon = new ipc::DaemonIpcClient(client, m_daemonIpcName);
  connect(daemon, &ipc::DaemonIpcClient::connected, client, [this, daemon, client, stopOld, generation] {
    if (generation != m_operationGeneration)
      return;
    daemon->sendStopProcess();
    QTimer::singleShot(250, client, stopOld);
  });
  connect(daemon, &ipc::DaemonIpcClient::versionMismatch, client, [this, client, done] {
    *done = true;
    qWarning("cannot stop existing core because the daemon version differs; stop it manually first");
    if (m_coreIpcClient == client) {
      m_coreIpcClient = nullptr;
      m_adoptedExistingCore = false;
      clearCoreStatus();
      setProcessState(ProcessState::Stopped);
    }
    client->disconnectFromServer();
    client->deleteLater();
  });
  connect(daemon, &ipc::DaemonIpcClient::connectionFailed, client, stopOld);
  QTimer::singleShot(1500, client, [daemon, stopOld] {
    if (!daemon->isConnected())
      stopOld();
  });
  daemon->connectToServer();
}

void CoreProcess::onProcessFinished(int exitCode, QProcess::ExitStatus)
{
  using enum ProcessState;
  clearCoreStatus();

  if (m_retryTimer.isActive()) {
    m_retryTimer.stop();
  }

  if (exitCode != s_exitSuccess) {
    setProcessState(Stopped);
    if (exitCode == s_exitDuplicate) {
      checkExistingProcess();
      return;
    }
    qWarning("desktop process exited with code: %d", exitCode);
    return;
  }

  qDebug("desktop process exited normally");

  if (const auto wasStarted = m_processState == Started; wasStarted) {
    qDebug("desktop process was running, retrying in %d ms", kRetryDelay);
    setProcessState(RetryPending);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.start(kRetryDelay);
  } else {
    setProcessState(Stopped);
  }
}

void CoreProcess::applyLogLevel()
{
  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>();
  if (processMode == ProcessMode::Service) {
    qDebug() << "setting daemon log level:" << Settings::logLevelText();
    m_daemonIpcClient->sendLogLevel(Settings::logLevelText());
  }
}

void CoreProcess::startForegroundProcess(const QStringList &args)
{
  using enum ProcessState;

  if (m_processState != Starting) {
    qCritical("not starting core desktop process, unexpected process state");
    return;
  }

  // only make quoted args for printing the command for convenience; so that the
  // core command can be easily copy/pasted to the terminal for testing.
  const auto quoted = makeQuotedArgs(m_appPath, args);
  qInfo("running command: %s", qPrintable(quoted));

#ifdef Q_OS_LINUX
  m_process->setChildProcessModifier([] {
    // the core process becomes orphaned when the gui process exits abruptly (e.g. with kill -9),
    // so ensure the os also kills the core when that happens to the gui.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
  });
#endif

  m_process->start(m_appPath, args);

  if (m_process->waitForStarted()) {
    setProcessState(Started);
  } else {
    setProcessState(Stopped);
    Q_EMIT error(Error::StartFailed);
  }
}

void CoreProcess::startProcessFromDaemon()
{
  if (m_processState != ProcessState::Starting) {
    qCritical("not starting core process from daemon, unexpected process state");
    return;
  }

  const auto configFile = Settings::settingsFile();
  qInfo("sending start to daemon (config file: %s)", qPrintable(configFile));

  const auto generation = m_operationGeneration;
  auto sendStart = [this, configFile, generation] {
    if (m_processState != ProcessState::Starting || generation != m_operationGeneration)
      return;
    m_daemonIpcClient->sendConfigFile(configFile);
    m_daemonIpcClient->sendStartProcess();
    setProcessState(ProcessState::Started);
  };

  if (m_daemonIpcClient->isConnected()) {
    sendStart();
  } else {
    connect(
        m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, sendStart,
        static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
    );
    m_daemonIpcClient->connectToServer();
  }
}

void CoreProcess::stopForegroundProcess()
{
  if (m_processState != ProcessState::Stopping) {
    qCritical("not stopping core desktop process, unexpected process state");
    return;
  }

  qInfo("stopping core desktop process");
  if (m_coreIpcClient && m_coreIpcClient->isConnected()) {
    qDebug("sending stop command to core process");
    if (m_adoptedExistingCore)
      stopExistingCoreSafely(m_coreIpcClient);
    else
      m_coreIpcClient->sendStop();

    // Fallback: if the core doesn't confirm shutdown via IPC within the
    // timeout, force-terminate it so we never hang in the Stopping state.
    QTimer::singleShot(m_adoptedExistingCore ? 3500 : kRetryDelay, this, [this] {
      if (m_processState != ProcessState::Stopping) {
        return;
      }

      qWarning("core process did not confirm graceful stop in time, forcing terminate");

      if (m_coreIpcClient) {
        m_coreIpcClient->disconnectFromServer();
        m_coreIpcClient->deleteLater();
        m_coreIpcClient = nullptr;
        m_adoptedExistingCore = false;
      }

      if (m_process && m_process->state() == QProcess::ProcessState::Running) {
        m_process->terminate();
      } else {
        setProcessState(ProcessState::Stopped);
      }
    });
    return;
  }

  if (!m_process) {
    qCritical("not stopping core desktop process, no process to stop");
    setProcessState(ProcessState::Stopped);
  } else if (m_process->state() == QProcess::ProcessState::Running) {
    qDebug("process is running, terminating");
    m_process->terminate();
  } else {
    qDebug("process is not running, skipping terminate");
  }
}

void CoreProcess::stopProcessFromDaemon()
{
  if (m_processState != ProcessState::Stopping) {
    qCritical("not stopping core process from daemon, unexpected process state");
    return;
  }

  const auto generation = m_operationGeneration;
  auto sendStop = [this, generation] {
    if (m_processState != ProcessState::Stopping || generation != m_operationGeneration)
      return;
    m_daemonIpcClient->sendStopProcess();
    setProcessState(ProcessState::Stopped);
  };

  if (m_daemonIpcClient->isConnected()) {
    sendStop();
  } else {
    connect(
        m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, sendStop,
        static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
    );
    m_daemonIpcClient->connectToServer();
  }
}

void CoreProcess::handleLogLines(const QString &text)
{
  const auto lines = text.split(kLineSplitRegex);
  for (const auto &line : lines) {
    if (line.isEmpty()) {
      continue;
    }

#if defined(Q_OS_MACOS)
    // HACK: macOS 10.13.4+ spamming error lines in logs making them
    // impossible to read and debug; giving users a red herring.
    if (line.contains("calling TIS/TSM in non-main thread environment")) {
      continue;
    }

    // the core process is not allowed to show the permission prompt
    // (called "notification permission") and the notification log line is emitted from
    // deep inside cocoa code in the core binary to stdout, so it can't be sent over
    // ipc from the core to the gui and instead the gui has to parse the core output.
    static const QString needle = "OSX Notification: ";
    if (line.contains(needle) && line.contains('|')) {
      const int delimiterPosition = line.indexOf('|');
      const int start = line.indexOf(needle);
      const QString title = line.mid(start + needle.length(), delimiterPosition - start - needle.length());
      const QString body = line.mid(delimiterPosition + 1, line.length() - delimiterPosition);
      if (!showOSXNotification(title, body)) {
        qDebug("osx notification was not shown");
      }
    }
#endif

    Q_EMIT logLine(line);
  }
}

void CoreProcess::start(std::optional<ProcessMode> processModeOption)
{
  if (m_processState == ProcessState::Started) {
    qCritical("not starting core process, already started");
    return;
  }

  if (m_mode == Settings::CoreMode::None) {
    qCritical("not starting core process, no core mode set");
    return;
  }

  QMutexLocker locker(&m_processMutex);

  ++m_operationGeneration;

  const auto currentMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  const auto processMode = processModeOption.value_or(currentMode);
  const auto coreMode = QVariant::fromValue(m_mode).toString().toLower();

  qInfo().noquote() << QString("starting %1 process (%2 mode)").arg(coreMode, processModeToString(processMode));

  setProcessState(ProcessState::Starting);

#ifdef Q_OS_MACOS
  requestOSXNotificationPermission();
#endif

  setConnectionState(ConnectionState::Connecting);

  // Reserve the same singleton as the core before touching the shared server
  // layout. An existing core may have acquired it before its IPC is listening.
  // Release after persistence so the newly launched core can acquire it.
  std::unique_ptr<QSharedMemory> layoutReservation;
  if (m_mode == Settings::CoreMode::Server && !Settings::value(Settings::Server::ExternalConfig).toBool()) {
    layoutReservation = std::make_unique<QSharedMemory>(m_singletonName);
    if (layoutReservation->attach())
      layoutReservation->detach(); // Match core's stale-segment cleanup.
    if (!layoutReservation->create(1)) {
      if (layoutReservation->error() == QSharedMemory::AlreadyExists) {
        checkExistingProcess();
      } else {
        qWarning("cannot reserve core singleton before saving server layout");
        setProcessState(ProcessState::Stopped);
        setConnectionState(ConnectionState::Disconnected);
        Q_EMIT error(Error::StartFailed);
      }
      return;
    }
  }

  if (processMode == ProcessMode::Desktop) {
    m_process = new QProcess(this);
    connect(m_process, &QProcess::finished, this, &CoreProcess::onProcessFinished, Qt::UniqueConnection);
    connect(
        m_process, &QProcess::readyReadStandardOutput, this, &CoreProcess::onProcessReadyReadStandardOutput,
        Qt::UniqueConnection
    );
    connect(
        m_process, &QProcess::readyReadStandardError, this, &CoreProcess::onProcessReadyReadStandardError,
        Qt::UniqueConnection
    );
  }

  QStringList args = {coreMode};

  if (m_mode == Settings::CoreMode::Server) {
    const auto [hasNeededPermissions, configFilename] = persistServerConfig();
    if (configFilename.isEmpty()) {
      qCritical("not starting core process, no server config file");
      setProcessState(ProcessState::Stopped);
      setConnectionState(ConnectionState::Disconnected);
      Q_EMIT error(Error::StartFailed);
      return;
    }
    if (!hasNeededPermissions) {
      setProcessState(ProcessState::Stopped);
      setConnectionState(ConnectionState::Disconnected);
      Q_EMIT error(Error::StartFailed);
      return;
    }
    qInfo("core config file: %s", qPrintable(configFilename));
  }
  // No existing core could have loaded a partially replaced layout while the
  // reservation was held. The launched process must acquire the singleton.
  if (layoutReservation)
    layoutReservation->detach();

  qDebug().noquote() << "log level:" << Settings::logLevelText();

  if (Settings::value(Settings::Log::ToFile).toBool()) {
    const auto logFile = Settings::value(Settings::Log::File).toString();
    QDir(QFileInfo(logFile).absolutePath()).mkpath(".");
    qInfo().noquote() << "log file:" << logFile;
  }

  // Wired before the start calls so it catches Started from both sync (desktop) and async (service) paths.
  connect(
      this, &CoreProcess::processStateChanged, this,
      [this](ProcessState state) {
        if (state != ProcessState::Started) {
          return;
        }

        // Delay briefly to give the core process time to start its IPC server.
        QTimer::singleShot(kRetryDelay, this, [this] {
          if (m_processState != ProcessState::Started) {
            return;
          }

          // The adopted connection owns its own identity-checked reconnect.
          // Its socket may be temporarily down when this delayed normal-start
          // callback runs; replacing it would bypass the adoption handshake.
          if (m_coreIpcClient && (m_adoptedExistingCore || m_coreIpcClient->isConnected()))
            return;
          if (m_coreIpcClient) {
            m_coreIpcClient->disconnectFromServer();
            m_coreIpcClient->deleteLater();
            m_coreIpcClient = nullptr;
          }

          m_coreIpcClient = new ipc::CoreIpcClient(this, m_coreIpcName);

          // Discard messages queued before a disconnect or a later start. A
          // queued callback can otherwise restore an expired online identity.
          const auto epoch = std::make_shared<std::uint64_t>(0);
          const auto generation = m_operationGeneration;
          connect(m_coreIpcClient, &ipc::CoreIpcClient::commandReceived, this,
                  [this, client = m_coreIpcClient, epoch, generation](const QString &command, const QString &args) {
            const auto receivedEpoch = *epoch;
            QMetaObject::invokeMethod(this, [this, client, epoch, generation, receivedEpoch, command, args] {
              if (m_coreIpcClient == client && m_operationGeneration == generation && *epoch == receivedEpoch)
                onCoreIpcMessageReceived(command, args);
            }, Qt::QueuedConnection);
          });
          connect(m_coreIpcClient, &ipc::CoreIpcClient::connected, this, [] {
            qDebug("connected to core ipc server");
          });
          connect(m_coreIpcClient, &ipc::CoreIpcClient::connectionFailed, this, [this, client = m_coreIpcClient, epoch] {
            if (m_coreIpcClient != client)
              return;
            ++*epoch;
            qWarning("core IPC disconnected; clearing online state and retrying");
            clearCoreStatus();
            QTimer::singleShot(kRetryDelay, this, [this, client] {
              if (m_coreIpcClient == client && m_processState == ProcessState::Started)
                client->connectToServer();
            });
          });
          connect(m_coreIpcClient, &ipc::CoreIpcClient::serverShutdown, this, [this, client = m_coreIpcClient, epoch] {
            qDebug("core ipc server shut down cleanly");
            if (m_coreIpcClient != client)
              return;
            ++*epoch;
            clearCoreStatus();
            if (m_processState == ProcessState::Started) {
              QTimer::singleShot(kRetryDelay, this, [this, client] {
                if (m_coreIpcClient == client && m_processState == ProcessState::Started)
                  client->connectToServer();
              });
            } else {
              client->deleteLater();
              m_coreIpcClient = nullptr;
            }
          });

          m_coreIpcClient->connectToServer();
        });
      },
      static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
  );

  if (processMode == ProcessMode::Desktop) {
    startForegroundProcess(args);
  } else if (processMode == ProcessMode::Service) {
    startProcessFromDaemon();
  }

  m_lastProcessMode = processMode;
}

void CoreProcess::stop(std::optional<ProcessMode> processModeOption)
{
  QMutexLocker locker(&m_processMutex);

  ++m_operationGeneration;

  const auto currentMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  const auto processMode = processModeOption.value_or(currentMode);

  qInfo("stopping core process (%s mode)", qPrintable(processModeToString(processMode)));

  if (m_coreIpcClient && processMode != ProcessMode::Desktop) {
    m_coreIpcClient->disconnectFromServer();
    m_coreIpcClient->deleteLater();
    m_coreIpcClient = nullptr;
  }

  if (m_processState == ProcessState::Starting) {
    qDebug("core process is starting, cancelling");
    setProcessState(ProcessState::Stopped);
  } else if (m_processState != ProcessState::Stopped) {
    setProcessState(ProcessState::Stopping);

    if (processMode == ProcessMode::Service) {
      stopProcessFromDaemon();
    } else if (processMode == ProcessMode::Desktop) {
      stopForegroundProcess();
    }

  } else {
    qWarning("core process already stopped");
  }

  clearCoreStatus();
}

void CoreProcess::restart()
{
  qDebug("restarting core process");

  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();

  if (m_adoptedExistingCore && m_coreIpcClient && m_coreIpcClient->isConnected()) {
    auto *client = m_coreIpcClient;
    const auto generation = m_operationGeneration + 1; // stop() advances it.
    auto completed = std::make_shared<bool>(false);
    auto restartAfterExit = [this, generation, completed] {
      if (std::exchange(*completed, true))
        return;
      QTimer::singleShot(0, this, [this, generation] {
        if (m_operationGeneration == generation && m_processState == ProcessState::Stopped)
          start();
      });
    };
    connect(client, &ipc::CoreIpcClient::serverShutdown, this, restartAfterExit);
    connect(client, &ipc::CoreIpcClient::connectionFailed, this, restartAfterExit);
    QTimer::singleShot(5000, this, [this, generation, completed] {
      if (generation == m_operationGeneration && !std::exchange(*completed, true))
        qWarning("existing core did not confirm exit; automatic restart was cancelled");
    });
    stop(m_lastProcessMode.value_or(processMode));
    return;
  }

  const bool waitForProcess = m_process && m_process->state() != QProcess::ProcessState::NotRunning;

  if (waitForProcess) {
    qInfo("waiting for current desktop core to exit before restarting");

    connect(
        m_process, &QProcess::finished, this,
        [this](int, QProcess::ExitStatus) {
          qInfo("desktop core exited, restarting");
          start();
        },
        Qt::SingleShotConnection
    );
  }

  if (m_lastProcessMode != std::nullopt && m_lastProcessMode != processMode) {
    const auto debugMessage =
        QStringLiteral("process mode changed to %1, stopping %2 process")
            .arg(processModeToString(processMode), processModeToString(m_lastProcessMode.value()));
    qDebug().noquote() << debugMessage;
    stop(m_lastProcessMode);
  } else {
    // in service mode: though there is technically no need to stop the service
    // before restarting it, it does make for cleaner process state tracking,
    // especially if something goes wrong with starting the service.
    stop();
  }

  if (!waitForProcess) {
    start();
  }
}

void CoreProcess::cleanup()
{
  qInfo("cleaning up core process");

  const auto isDesktop = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>() == ProcessMode::Desktop;
  const auto isRunning = m_processState == ProcessState::Started;
  if (isDesktop && isRunning) {
    stop();
  }
}

QPair<bool, QString> CoreProcess::persistServerConfig() const
{
  if (Settings::value(Settings::Server::ExternalConfig).toBool()) {
    return {Settings::isServerConfigFileReadable(), Settings::value(Settings::Server::ExternalConfigFile).toString()};
  }

  const auto configFilePath = Settings::defaultValue(Settings::Server::ExternalConfigFile).toString();
  QFile configFile(configFilePath);
  if (!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    qWarning() << "failed to open core config file for write:" << configFilePath;
    return {false, configFile.fileName()};
  }

  m_serverConfig.save(configFile);
  configFile.close();
  return {Settings::isServerConfigFileReadable(), configFile.fileName()};
}

void CoreProcess::setConnectionState(ConnectionState state)
{
  if (m_connectionState == state) {
    return;
  }

  m_connectionState = state;
  Q_EMIT connectionStateChanged(state);
}

void CoreProcess::clearCoreStatus()
{
  setConnectionState(ConnectionState::Disconnected);
  Q_EMIT connectedClientsChanged({});
  Q_EMIT keyboardClientIdentitiesChanged({});
  Q_EMIT connectedServerAddressChanged({});
  Q_EMIT keyboardServerFingerprintChanged({});
}

void CoreProcess::setProcessState(ProcessState state)
{
  if (m_processState == state) {
    return;
  }

  qDebug(
      "core process state changed: %s -> %s", //
      qPrintable(processStateToString(m_processState)), qPrintable(processStateToString(state))
  );
  m_processState = state;
  Q_EMIT processStateChanged(state);
}

void CoreProcess::onCoreIpcMessageReceived(const QString &command, const QString &args)
{
  if (command == "connectionState") {
    const auto metaEnum = QMetaEnum::fromType<ConnectionState>();
    bool ok = false;
    const auto state = static_cast<ConnectionState>(metaEnum.keyToValue(args.toUtf8().constData(), &ok));
    if (!ok) {
      qWarning("core ipc got unknown connection state: %s", args.toUtf8().constData());
      return;
    }
    setConnectionState(state);
  } else if (command == "connectedClients") {
    const auto clients = args.isEmpty() ? QStringList() : args.split(",");
    Q_EMIT connectedClientsChanged(clients);
  } else if (command == "keyboardClientIdentities") {
    Q_EMIT keyboardClientIdentitiesChanged(args);
  } else if (command == "serverAddress") {
    Q_EMIT connectedServerAddressChanged(args);
  } else if (command == "keyboardServerFingerprint") {
    Q_EMIT keyboardServerFingerprintChanged(args);
  } else if (command == "secureSocket") {
    Q_EMIT secureSocket(true);
    if (args != m_secureSocketVersion) {
      m_secureSocketVersion = args;
      Q_EMIT securityLevelChanged(args);
    }
  } else if (command == "unrecognisedClient") {
    Q_EMIT unrecognisedClient(args);
  } else if (command == "connectionRefused") {
    const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionRefusal>();
    bool ok = false;
    const auto reason =
        static_cast<deskflow::core::ConnectionRefusal>(metaEnum.keyToValue(args.toUtf8().constData(), &ok));
    if (ok) {
      Q_EMIT connectionRefused(reason);
    } else {
      qWarning("core ipc got unknown connection refusal: %s", args.toUtf8().constData());
    }
  } else if (command == "retryIn") {
    Q_EMIT retryIn(args.toInt());
  } else if (command == "peerFingerprint") {
    Q_EMIT peerFingerprint(args);
  } else if (command == "missingKeyboardLayouts") {
    Q_EMIT missingKeyboardLayouts(args);
  }
}

bool CoreProcess::checkSecureSocket(const QString &line)
{
  static const QString tlsCheckString = "network encryption protocol: ";
  const auto index = line.indexOf(tlsCheckString, 0, Qt::CaseInsensitive);
  if (index == -1) {
    return false;
  }

  Q_EMIT secureSocket(true);
  if (const auto ssv = line.mid(index + tlsCheckString.size()); ssv != m_secureSocketVersion) {
    m_secureSocketVersion = ssv;
    Q_EMIT securityLevelChanged(ssv);
  }

  return true;
}

QString CoreProcess::correctedAddress(const QString &address) const
{
  return wrapIpv6(address.simplified());
}

void CoreProcess::setupDaemonLogTail(const QString &logPath)
{
  qDebug() << "daemon log path:" << logPath;

  if (QFileInfo logFile(logPath); !logFile.isFile()) {
    auto file = QFile(logPath);
    if (!file.open(QFile::ReadWrite)) {
      qCritical() << "daemon log path file can not be written:" << logPath;
      return;
    }
    file.write(""); // Create an empty file
  }

  if (m_daemonFileTail) {
    m_daemonFileTail->setWatchedFile(logPath);
  } else {
    m_daemonFileTail = new FileTail(logPath, this);
    connect(m_daemonFileTail, &FileTail::newLine, this, &CoreProcess::handleLogLines);
  }
}

void CoreProcess::clearSettings()
{
  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  if (processMode == ProcessMode::Desktop) {
    qDebug("no core settings to clear in desktop mode");
    return;
  }

  if (processMode != ProcessMode::Service) {
    qCritical("not clearing core settings, unexpected process mode");
    return;
  }

  qInfo("clearing core settings through daemon");
  m_daemonIpcClient->sendClearSettings();
}

void CoreProcess::retryDaemon()
{
  m_daemonIpcClient->connectToServer();
}

} // namespace deskflow::gui
