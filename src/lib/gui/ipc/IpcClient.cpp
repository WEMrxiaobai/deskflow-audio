/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcClient.h"

#include "common/VersionInfo.h"
#include "common/Constants.h"
#include "WindowsIpcPeer.h"
#include <QCoreApplication>
#include <QDir>

#include <QDebug>
#include <QLocalSocket>
#include <QTimer>

namespace deskflow::gui::ipc {
namespace {
constexpr qint64 kMaxMessageBytes = 65536; // Includes newline.
}

IpcClient::IpcClient(QObject *parent, const QString &socketName, const QString &typeName)
    : QObject(parent),
      m_socket{new QLocalSocket(this)},
      m_socketName(socketName), // NOSONAR - Qt memory
      m_typeName(typeName)
{
  m_socket->setReadBufferSize(kMaxMessageBytes);
  connect(m_socket, &QLocalSocket::disconnected, this, &IpcClient::handleDisconnected);
  connect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleErrorOccurred);
  connect(m_socket, &QLocalSocket::readyRead, this, &IpcClient::handleReadyRead);
}

void IpcClient::connectToServer()
{
  if (m_state == State::Connecting) {
    qWarning().noquote() << QStringLiteral("%1 ipc client already connecting to server").arg(m_typeName);
    return;
  }

  if (m_state != State::Unconnected) {
    qDebug().noquote() << QStringLiteral("%1 ipc client not in unconnected state, disconnecting").arg(m_typeName);
    disconnectFromServer();
  }

  if (m_socket->state() != QLocalSocket::UnconnectedState) {
    qWarning().noquote(
    ) << QStringLiteral("%1 ipc client socket not in unconnected state, disconnecting").arg(m_typeName);
    disconnectFromServer();
  }

  m_retryCount = 0;
  attemptConnection();
}

void IpcClient::attemptConnection()
{
  if (const int retryLimit = 10; m_retryCount >= retryLimit) {
    qWarning().noquote() << QStringLiteral("%1 ipc client failed to connect after %2 attempts")
                                .arg(m_typeName, QString::number(retryLimit));
    m_state = State::Unconnected;
    Q_EMIT connectionFailed();
    return;
  }

  if (m_retryCount == 0) {
    qDebug().noquote() << QStringLiteral("%1 ipc client connecting to server: %2").arg(m_typeName, m_socketName);
  } else {
    qDebug().noquote() << QStringLiteral("%1 ipc client retrying connection, attempt: %2")
                              .arg(m_typeName, QString::number(m_retryCount + 1));
  }

  m_state = State::Connecting;
  m_retryCount++;

  connect(
      m_socket, &QLocalSocket::connected, this, &IpcClient::handleConnectionSuccess,
      static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::UniqueConnection)
  );

  connect(
      m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleConnectionFailure,
      static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::UniqueConnection)
  );

  m_socket->connectToServer(m_socketName);
}

void IpcClient::disconnectFromServer()
{
  m_state = State::Disconnecting;
  qDebug().noquote() << QStringLiteral("%1 ipc client disconnecting from server").arg(m_typeName);
  m_socket->disconnectFromServer();
  m_state = State::Unconnected;
}

void IpcClient::handleDisconnected()
{
  if (m_state == State::Connecting) {
    return;
  }

  qDebug().noquote() << QStringLiteral("%1 ipc client disconnected from server").arg(m_typeName);
  const auto wasConnected = m_state == State::Connected;
  m_state = State::Unconnected;

  if (wasConnected) {
    Q_EMIT connectionFailed();
  }
}

void IpcClient::handleErrorOccurred()
{
  if (m_state == State::Connecting) {
    return;
  }

  qWarning().noquote() << QStringLiteral("%1 ipc client error: %2").arg(m_typeName, m_socket->errorString());

  if (m_state == State::Connected) {
    disconnectFromServer();
    Q_EMIT connectionFailed();
  }
}

void IpcClient::handleReadyRead()
{
  while (m_socket->canReadLine()) {
    const auto data = m_socket->readLine(kMaxMessageBytes + 1);
    if (data.size() > kMaxMessageBytes || !data.endsWith('\n')) {
      disconnectFromServer();
      Q_EMIT connectionFailed();
      return;
    }
    const auto message = QString::fromUtf8(data.constData(), data.size() - 1);

    const auto parts = message.split('=');
    if (parts.isEmpty()) {
      qWarning().noquote() << QStringLiteral("%1 ipc client got invalid message").arg(m_typeName);
      continue;
    }
    qDebug().noquote() << QStringLiteral("%1 ipc client command: %2").arg(m_typeName, parts.at(0));

    if (m_state == State::Connecting) {
      handleHandshakeMessage(parts);
      if (m_state == State::Unconnected)
        return;
      continue;
    }

    if (parts.at(0) == QStringLiteral("bye")) {
      qDebug().noquote() << QStringLiteral("%1 ipc server is shutting down").arg(m_typeName);
      disconnectFromServer();
      Q_EMIT serverShutdown();
      return;
    }

    processCommand(parts.at(0), parts);
  }

  if (m_socket->bytesAvailable() >= kMaxMessageBytes) {
    qWarning().noquote() << QStringLiteral("%1 ipc server message exceeds limit").arg(m_typeName);
    disconnectFromServer();
    Q_EMIT connectionFailed();
  }
}

void IpcClient::handleHandshakeMessage(const QStringList &parts)
{
  if (parts.at(0) == QStringLiteral("error")) {
    const auto detail = parts.size() >= 2 ? parts.at(1) : QStringLiteral("unknown");
    qCritical().noquote() << QStringLiteral("%1 ipc server rejected connection: %2").arg(m_typeName, detail);
    disconnectFromServer();
    Q_EMIT connectionFailed();
    return;
  }

  const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);

  if (parts.at(0) == QStringLiteral("versionMismatch")) {
    const auto serverVersion = parts.size() >= 2 ? parts.at(1) : QStringLiteral("unknown");
    qWarning().noquote(
    ) << QStringLiteral("%1 ipc version mismatch (client: %2, server: %3)").arg(m_typeName, versionId, serverVersion);
    m_state = State::Connected;
    Q_EMIT versionMismatch();
    return;
  }

  if (parts.at(0) != QStringLiteral("hello")) {
    return;
  }

  if (parts.size() < 2 || parts.at(1) != versionId) {
    qCritical().noquote() << QStringLiteral("%1 ipc server sent an invalid hello version").arg(m_typeName);
    disconnectFromServer();
    Q_EMIT connectionFailed();
    return;
  }

  m_state = State::Connected;
  qDebug().noquote() << QStringLiteral("%1 ipc client connected").arg(m_typeName);
  Q_EMIT connected();
}

void IpcClient::handleConnectionFailure()
{
  qWarning().noquote(
  ) << QStringLiteral("%1 ipc client failed to connect: %2").arg(m_typeName, m_socket->errorString());
  m_socket->disconnectFromServer();
  m_state = State::Unconnected;
  QTimer::singleShot(500, this, &IpcClient::attemptConnection);
}

void IpcClient::handleConnectionSuccess()
{
#ifdef Q_OS_WIN
  // Production names are fixed; injected names are reserved for isolated tests.
  const bool daemon = m_socketName == QLatin1String(kDaemonIpcName);
  const bool core = m_socketName == QLatin1String(kCoreIpcName);
  if ((daemon || core) && !detail::verifyPipeServer(
          m_socket, daemon, QDir(QCoreApplication::applicationDirPath()).filePath(QLatin1String(kCoreBinName)))) {
    qWarning().noquote() << QStringLiteral("%1 ipc server identity could not be verified").arg(m_typeName);
    m_socket->abort();
    m_state = State::Unconnected;
    Q_EMIT connectionFailed();
    return;
  }
#endif
  const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
  m_socket->write(QStringLiteral("hello=%1\n").arg(versionId).toUtf8());
  qDebug().noquote() << QStringLiteral("%1 ipc client sent hello with version: %2").arg(m_typeName, versionId);
}

void IpcClient::sendMessage(const QString &message)
{
  if (m_state != State::Connected) {
    qWarning().noquote() << QStringLiteral("%1 cannot send command, ipc client not connected").arg(m_typeName);
    return;
  }

  m_socket->write(message.toUtf8() + "\n");
  qDebug().noquote() << QStringLiteral("%1 ipc client sent command: %2").arg(m_typeName, message.section('=', 0, 0));
}

} // namespace deskflow::gui::ipc
