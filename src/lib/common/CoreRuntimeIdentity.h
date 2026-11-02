/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once

#include "Settings.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>

inline void setCoreRuntimeLayout(QJsonObject &identity, const QByteArray &bytes)
{
  const bool valid = bytes.size() <= 1024 * 1024;
  identity["configValid"] = valid;
  if (valid)
    identity["configHash"] = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  else
    identity.remove("configHash");
}

// Capture the settings which select the running core's role and peer. Server
// layouts are loaded once, so compare their contents rather than their path.
inline QJsonObject coreRuntimeIdentity(bool server)
{
  QJsonObject result{
      {"mode", server ? QStringLiteral("server") : QStringLiteral("client")},
      {"name", Settings::value(Settings::Core::ComputerName).toString()},
      {"port", Settings::value(Settings::Core::Port).toInt()},
      {"tls", Settings::value(Settings::Security::TlsEnabled).toBool()}
  };
  if (!server) {
    result["target"] = Settings::value(Settings::Client::RemoteHost).toString().trimmed();
  } else {
    QFile file(Settings::serverConfigFile());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
      result["configValid"] = false;
    } else {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      const bool valid = hash.addData(&file);
      result["configValid"] = valid;
      result["configHash"] = QString::fromLatin1(hash.result().toHex());
    }
  }
  return result;
}
