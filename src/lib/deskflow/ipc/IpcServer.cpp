/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcServer.h"
#include "WindowsIpcAccess.h"

#include "base/Log.h"
#include "common/VersionInfo.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace deskflow::core::ipc {
namespace {
// Events are delivered once to the first GUI that attaches. Bound both count
// and storage: some events contain peer-controlled names or fingerprints.
constexpr qsizetype kMaxPendingEvents = 64;
constexpr qsizetype kMaxPendingCharacters = 32768;
constexpr qint64 kMaxMessageBytes = 65536; // Includes newline.
}

IpcServer::IpcServer(QObject *parent, const QString &serverName, const QString &typeName)
    : QObject(parent),
      m_server{new QLocalServer(this)}, // NOSONAR - Qt memory
      m_serverName(serverName),
      m_typeName(typeName.toUtf8())
{
  // do nothing
}

IpcServer::~IpcServer()
{
  m_server->close();
}

void IpcServer::listen()
{
  // IPC server normally runs as system, but GUI runs as regular user, so we need to allow world access.
  m_server->setSocketOptions(QLocalServer::WorldAccessOption);

  connect(m_server, &QLocalServer::newConnection, this, &IpcServer::handleNewConnection);
  QLocalServer::removeServer(m_serverName);
  if (m_server->listen(m_serverName)) {
    LOG_DEBUG("%s ipc server listening on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  } else {
    LOG_ERR("%s ipc server failed to listen on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  }
}

void IpcServer::handleNewConnection()
{
  QLocalSocket *clientSocket = m_server->nextPendingConnection();
  if (!clientSocket) {
    LOG_ERR("%s ipc server failed to get new connection", m_typeName.constData());
    return;
  }

  LOG_DEBUG("%s ipc server got new connection", m_typeName.constData());
#ifdef Q_OS_WIN
  if ((m_typeName == "core" || m_typeName == "daemon") &&
      !detail::authorizePipe(clientSocket, m_typeName == "core" ? detail::AccessServer::Core : detail::AccessServer::Daemon)) {
    LOG_WARN("%s ipc server rejected client session", m_typeName.constData());
    clientSocket->abort();
    clientSocket->deleteLater();
    return;
  }
#endif
  m_clients.insert(clientSocket);
  clientSocket->setReadBufferSize(kMaxMessageBytes);

  connect(clientSocket, &QLocalSocket::readyRead, this, &IpcServer::handleReadyRead);
  connect(clientSocket, &QLocalSocket::disconnected, this, &IpcServer::handleDisconnected);
  connect(clientSocket, &QLocalSocket::errorOccurred, this, &IpcServer::handleErrorOccurred);
}

void IpcServer::handleReadyRead()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_VERBOSE("%s ipc server ready to read data", m_typeName.constData());

  // QLocalSocket owns a separate bounded buffer for each client. Keep partial
  // lines there until their delimiter arrives, including after complete lines.
  while (clientSocket->canReadLine()) {
    const auto data = clientSocket->readLine(kMaxMessageBytes + 1);
    if (data.size() > kMaxMessageBytes || !data.endsWith('\n')) {
      clientSocket->abort();
      return;
    }
#ifdef Q_OS_WIN
    if ((m_typeName == "core" || m_typeName == "daemon") &&
        !detail::authorizePipe(clientSocket, m_typeName == "core" ? detail::AccessServer::Core : detail::AccessServer::Daemon)) {
      clientSocket->abort();
      return;
    }
#endif
    processMessage(clientSocket, QString::fromUtf8(data.constData(), data.size() - 1));
    if (clientSocket->state() != QLocalSocket::ConnectedState)
      return;
  }
  if (clientSocket->bytesAvailable() >= kMaxMessageBytes) {
    LOG_WARN("%s ipc client message exceeds limit", m_typeName.constData());
    clientSocket->abort();
  }
}

void IpcServer::handleDisconnected()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_DEBUG("%s ipc server client disconnected", m_typeName.constData());
  m_clients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::handleErrorOccurred()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  if (clientSocket->error() == QLocalSocket::PeerClosedError) {
    LOG_DEBUG("%s ipc server client closed connection", m_typeName.constData());
  } else {
    LOG_ERR("%s ipc server client error: %s", m_typeName.constData(), clientSocket->errorString().toUtf8().constData());
  }

  m_clients.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::processMessage(QLocalSocket *clientSocket, const QString &message)
{
  LOG_VERBOSE("%s ipc server got message: %s", m_typeName.constData(), message.toUtf8().constData());
  const auto parts = message.split('=');
  if (parts.isEmpty()) {
    LOG_ERR("%s ipc server got invalid message: %s", m_typeName.constData(), message.toUtf8().constData());
    writeToClientSocket(clientSocket, QStringLiteral("error"));
    return;
  }

  if (const auto &command = parts.at(0); command == QStringLiteral("hello")) {
    if (parts.size() < 2) {
      LOG_ERR("%s ipc client hello missing version", m_typeName.constData());
      writeToClientSocket(clientSocket, "error=missing version");
      clientSocket->flush();
      clientSocket->disconnectFromServer();
      return;
    }

    const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
    const auto clientVersion = parts.at(1);
    LOG_DEBUG("%s ipc server got hello message (version: %s)", m_typeName.constData(), versionId.toUtf8().constData());

    if (clientVersion != versionId) {
      LOG_WARN(
          "%s ipc client version mismatch (client: %s, server: %s)", m_typeName.constData(),
          clientVersion.toUtf8().constData(), versionId.toUtf8().constData()
      );
      writeToClientSocket(clientSocket, QStringLiteral("versionMismatch=%1").arg(versionId));
      clientSocket->flush();
      return;
    }

    LOG_DEBUG("%s ipc server sending hello back", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("hello=%1").arg(versionId));

    // Replay messages that were queued before any clients connected.
    LOG_VERBOSE("ipc server replaying %d pending messages", m_pendingMessages.size());
    for (const auto &pending : std::as_const(m_pendingMessages)) {
      LOG_VERBOSE("%s ipc server replaying: %s", m_typeName.constData(), pending.toUtf8().constData());
      writeToClientSocket(clientSocket, pending);
    }
    m_pendingMessages.clear();
    m_pendingCharacters = 0;
    // A newly attached GUI needs the current state even if another GUI already
    // consumed the one-time pending messages while the core kept running.
    for (const auto &status : std::as_const(m_coreStatus))
      writeToClientSocket(clientSocket, status);
  } else if (command == QStringLiteral("noop")) {
    LOG_DEBUG("%s ipc server got noop message", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
  } else {
    processCommand(clientSocket, command, parts);
  }

  clientSocket->flush();
}

void IpcServer::broadcastCommand(const QString &command, const QString &args)
{
  const auto message = args.isEmpty() ? command : QStringLiteral("%1=%2").arg(command, args);

  const bool isCoreStatus = m_typeName == "core" &&
      (command == QStringLiteral("coreIdentity") || command == QStringLiteral("connectionState") ||
       command == QStringLiteral("connectedClients") || command == QStringLiteral("keyboardClientIdentities") ||
       command == QStringLiteral("serverAddress") || command == QStringLiteral("keyboardServerFingerprint") ||
       command == QStringLiteral("secureSocket"));
  if (isCoreStatus)
    m_coreStatus.insert(command, message);

  if (m_clients.isEmpty()) {
    // State is replayed from its latest snapshot, never from event history.
    // A countdown is stale by the time a GUI attaches; later updates still
    // broadcast live.
    if (isCoreStatus || (m_typeName == "core" && command == QStringLiteral("retryIn")))
      return;
    if (message.size() > kMaxPendingCharacters)
      return;
    while (!m_pendingMessages.isEmpty() &&
           (m_pendingMessages.size() >= kMaxPendingEvents ||
            m_pendingCharacters + message.size() > kMaxPendingCharacters)) {
      m_pendingCharacters -= m_pendingMessages.takeFirst().size();
    }
    LOG_VERBOSE(
        "%s ipc server has no clients, message queued: %s", m_typeName.constData(), message.toUtf8().constData()
    );
    m_pendingMessages.append(message);
    m_pendingCharacters += message.size();
    return;
  }

  LOG_VERBOSE(
      "%s ipc server broadcasting message to %d clients: %s", m_typeName.constData(), m_clients.size(),
      message.toUtf8().constData()
  );
  // A daemon client can become inactive after connecting; do not continue
  // delivering state to it after a user/session switch.
  const auto clients = m_clients;
  for (auto *client : clients) {
#ifdef Q_OS_WIN
    if ((m_typeName == "core" || m_typeName == "daemon") &&
        !detail::authorizePipe(client, m_typeName == "core" ? detail::AccessServer::Core : detail::AccessServer::Daemon)) {
      client->abort();
      continue;
    }
#endif
    writeToClientSocket(client, message);
    client->flush();
  }
}

void IpcServer::writeToClientSocket(QLocalSocket *&clientSocket, const QString &message) const
{
  QByteArray messageData = message.toUtf8() + '\n';
  qint64 bytesWritten = clientSocket->write(messageData);
  if (bytesWritten != messageData.size()) {
    LOG_ERR("%s ipc server failed to write full message to client socket", m_typeName.constData());
  } else {
    LOG_VERBOSE(
        "%s ipc server wrote message to client socket: %s", m_typeName.constData(), message.toUtf8().constData()
    );
  }
}

} // namespace deskflow::core::ipc
