/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Isolated transport/security tests: no installed settings, real audio, core,
// service, or trust changes. Pass the openssl executable as the first argument.
#include "gui/audio/PairedAudioControl.h"
#include "net/FingerprintDatabase.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>

namespace {
bool waitFor(const std::function<bool()> &predicate, int timeout = 7000)
{
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents();
    if (predicate())
      return true;
    QThread::msleep(5);
  } while (timer.elapsed() < timeout);
  return predicate();
}
bool generate(const QString &openssl, const QString &pem, const QString &name)
{
  QProcess process;
  process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  process.start(openssl, {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"),
                        QStringLiteral("rsa:2048"), QStringLiteral("-nodes"), QStringLiteral("-days"),
                        QStringLiteral("1"), QStringLiteral("-subj"), QStringLiteral("/CN=") + name,
                        QStringLiteral("-keyout"), pem, QStringLiteral("-out"), pem + QStringLiteral(".crt")});
  if (!process.waitForFinished(15000) || process.exitCode() != 0)
    return false;
  QFile cert(pem + QStringLiteral(".crt")), combined(pem);
  if (!cert.open(QIODevice::ReadOnly) || !combined.open(QIODevice::Append))
    return false;
  return combined.write(cert.readAll()) > 0;
}
Fingerprint fingerprint(const QString &pem)
{
  QFile file(pem);
  file.open(QIODevice::ReadOnly);
  const QSslCertificate cert(file.readAll(), QSsl::Pem);
  return {QCryptographicHash::Sha256, cert.digest(QCryptographicHash::Sha256)};
}
bool trust(const QString &path, const QString &pem)
{
  FingerprintDatabase db;
  if (!pem.isEmpty())
    db.addTrusted(fingerprint(pem));
  return db.write(path);
}
} // namespace
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc != 2) {
    std::cerr << "usage: paired-control-tests <openssl-executable>\n";
    return 1;
  }
  QTemporaryDir directory;
  if (!directory.isValid())
    return 2;
  const auto alicePem = directory.filePath(QStringLiteral("alice.pem"));
  const auto bobPem = directory.filePath(QStringLiteral("bob.pem"));
  const auto malloryPem = directory.filePath(QStringLiteral("mallory.pem"));
  const auto aliceDb = directory.filePath(QStringLiteral("alice-trusted-clients"));
  const auto bobDb = directory.filePath(QStringLiteral("bob-trusted-servers"));
  if (!generate(QString::fromLocal8Bit(argv[1]), alicePem, QStringLiteral("alice")) ||
      !generate(QString::fromLocal8Bit(argv[1]), bobPem, QStringLiteral("bob")) ||
      !generate(QString::fromLocal8Bit(argv[1]), malloryPem, QStringLiteral("mallory")) ||
      !trust(aliceDb, bobPem) || !trust(bobDb, alicePem))
    return 3;
  QTcpServer probe;
  if (!probe.listen(QHostAddress::LocalHost, 0))
    return 4;
  const auto port = probe.serverPort();
  probe.close();
  PairedAudioControl alice, bob;
  PairedAudioControl::Configuration a, b;
  a.useSettings = b.useSettings = false;
  a.certificatePath = alicePem;
  b.certificatePath = bobPem;
  a.trustedClientsPath = aliceDb;
  b.trustedServersPath = bobDb;
  a.localName = QStringLiteral("alice");
  b.localName = QStringLiteral("bob");
  a.port = b.port = port;
  alice.setConfiguration(a);
  bob.setConfiguration(b);
  int received = 0, aliceDown = 0;
  QObject::connect(&alice, &PairedAudioControl::messageReceived, [&](const QString &, const QJsonObject &object) {
    if (object.value(QStringLiteral("test")) == QStringLiteral("confidential"))
      ++received;
  });
  QObject::connect(&alice, &PairedAudioControl::peerDisconnected, [&](const QString &) { ++aliceDown; });
  const auto host = QStringLiteral("127.0.0.1:%1").arg(24800);
  // Both certificates are trusted, but only Bob owns the live keyboard name.
  FingerprintDatabase trustedClients;
  trustedClients.read(aliceDb);
  trustedClients.addTrusted(fingerprint(malloryPem));
  if (!trustedClients.write(aliceDb)) return 20;
  const auto aliceId = QString::fromLatin1(fingerprint(alicePem).data.toHex());
  const auto bobId = QString::fromLatin1(fingerprint(bobPem).data.toHex());
  const auto malloryId = QString::fromLatin1(fingerprint(malloryPem).data.toHex());
  b.certificatePath = malloryPem;
  bob.setConfiguration(b);
  alice.configure(true, true, {}, {{QStringLiteral("bob"), bobId}});
  bob.configure(false, true, host, {}, aliceId);
  if (waitFor([&] { return !alice.peerIds().isEmpty(); }, 2500)) return 21;
  bob.configure(false, false, host);
  b.certificatePath = bobPem;
  bob.setConfiguration(b);
  // The authenticated fingerprint and the online name must both match.
  alice.configure(true, true, {}, {{QStringLiteral("not-bob"), bobId}});
  bob.configure(false, true, host, {}, aliceId);
  if (waitFor([&] { return !alice.peerIds().isEmpty(); }, 2500))
    return 5;
  alice.configure(true, true, {}, {{QStringLiteral("bob"), bobId}});
  if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; }))
    return 6;
  if (alice.peerIds().first() != QString::fromLatin1(fingerprint(bobPem).data.toHex()) ||
      bob.peerIds().first() != QString::fromLatin1(fingerprint(alicePem).data.toHex()) ||
      alice.peerAddress(alice.peerIds().first()).isEmpty())
    return 7;
  // Slow TLS handshakes from one address cannot fill every slot, evict an
  // authenticated peer, or prevent that peer reconnecting after a short wait.
  {
    QTcpSocket stalled[12];
    for (auto &socket : stalled)
      socket.connectToHost(QHostAddress::LocalHost, port);
    waitFor([&] { return stalled[0].state() == QAbstractSocket::ConnectedState; }, 2000);
    if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; }, 2000)) return 30;
    bob.configure(false, false, host);
    if (!waitFor([&] { return alice.peerIds().isEmpty(); })) return 31;
    bob.configure(false, true, host, {}, aliceId);
    if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; }, 8000)) return 32;
  }
  // A keyboard disconnect or reconnect under a different certificate revokes
  // the old audio socket immediately, even while both certificates are trusted.
  alice.configure(true, true, {}, {{QStringLiteral("bob"), malloryId}});
  if (!alice.peerIds().isEmpty() || !waitFor([&] { return bob.peerIds().isEmpty(); })) return 22;
  if (waitFor([&] { return !alice.peerIds().isEmpty(); }, 2500)) return 23;
  alice.configure(true, true, {}, {{QStringLiteral("bob"), bobId}});
  if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; })) return 24;
  bob.send(bob.peerIds().first(), {{QStringLiteral("test"), QStringLiteral("confidential")}});
  if (!waitFor([&] { return received == 1; }))
    return 8;
  // The same address can host another trusted certificate. It must not be
  // accepted unless it is the one on the active keyboard connection.
  FingerprintDatabase trustedServers;
  trustedServers.read(bobDb);
  trustedServers.addTrusted(fingerprint(malloryPem));
  if (!trustedServers.write(bobDb)) return 25;
  bob.configure(false, true, host, {}, malloryId);
  if (!bob.peerIds().isEmpty() || !waitFor([&] { return alice.peerIds().isEmpty(); })) return 26;
  if (waitFor([&] { return !bob.peerIds().isEmpty(); }, 2500)) return 27;
  bob.configure(false, true, host, {}, {});
  if (waitFor([&] { return !bob.peerIds().isEmpty(); }, 1500)) return 28;
  bob.configure(false, true, host, {}, aliceId);
  if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; })) return 29;
  // Plaintext is not interpreted as a control message or another identity.
  QTcpSocket plaintext;
  plaintext.connectToHost(QHostAddress::LocalHost, port);
  if (!waitFor([&] { return plaintext.state() == QAbstractSocket::ConnectedState; }))
    return 9;
  plaintext.write("{\"test\":\"confidential\"}\n");
  if (!waitFor([&] { return plaintext.state() == QAbstractSocket::UnconnectedState; }) || received != 1 ||
      alice.peerIds().size() != 1)
    return 10;
  // An oversized local message closes rather than allocating an unbounded queue.
  bob.send(bob.peerIds().first(), {{QStringLiteral("test"), QString(5000, 'x')}});
  if (!waitFor([&] { return aliceDown > 0; }))
    return 11;
  if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; }))
    return 12;
  // Revocation applies to an established TLS socket, not only the next handshake.
  if (!trust(aliceDb, {}))
    return 13;
  if (!waitFor([&] { return alice.peerIds().isEmpty() && bob.peerIds().isEmpty(); }))
    return 14;
  if (waitFor([&] { return !alice.peerIds().isEmpty(); }, 2500))
    return 15;
  if (!trust(aliceDb, bobPem))
    return 16;
  if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; }))
    return 17;
  // Losing the keyboard readiness lease tears down audio immediately.
  alice.configure(true, false, {}, {{QStringLiteral("bob"), bobId}});
  if (!alice.peerIds().isEmpty() || !waitFor([&] { return bob.peerIds().isEmpty(); }))
    return 18;
  // TLS without original client-certificate checking must never enable listening.
  a.checkPeers = false;
  alice.setConfiguration(a);
  alice.configure(true, true, {}, {{QStringLiteral("bob"), bobId}});
  if (waitFor([&] { return !alice.peerIds().isEmpty(); }, 2500))
    return 19;
  bob.configure(false, false, host);
  std::cout << "mutual_tls=pass identity_pin=pass original_name_gate=pass plaintext=reject "
               "bounded_frame=pass revocation=pass reconnect=pass keyboard_gate=pass unchecked_tls=reject\n";
  return 0;
}
