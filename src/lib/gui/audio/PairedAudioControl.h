/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <memory>

// An optional audio control channel authenticated ONLY by the original Deskflow
// certificate and role-specific trust database. Never creates identities/trust.
// All methods must run on this object's Qt thread. No message bodies are logged.
class PairedAudioControl : public QObject
{
  Q_OBJECT
public:
  struct Configuration
  {
    // Production always uses Settings. Explicit profiles permit isolated tests
    // without touching the installed identity or trust databases.
    bool useSettings = true;
    QString certificatePath;
    QString privateKeyPath; // Empty means the combined certificate PEM.
    QString trustedClientsPath;
    QString trustedServersPath;
    QString localName;
    quint16 port = 24801;
    bool tlsEnabled = true;
    bool checkPeers = true;
  };

  explicit PairedAudioControl(QObject *parent = nullptr);
  ~PairedAudioControl() override;
  void setConfiguration(const Configuration &configuration);
  void configure(bool server, bool keyboardReady, const QString &serverHost, const QStringList &connectedNames = {});
  void send(const QString &id, const QJsonObject &message);
  QStringList peerIds() const;
  QString peerAddress(const QString &id) const;
  bool isServer() const;

Q_SIGNALS:
  void peerConnected(const QString &id, const QString &name);
  void peerDisconnected(const QString &id);
  void messageReceived(const QString &id, const QJsonObject &message);
  void statusChanged(const QString &status);

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
