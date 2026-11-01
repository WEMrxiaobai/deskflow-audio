/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Complete original-identity mTLS -> memory offer/answer -> SRTP/Opus test.
// Uses temporary original-style identities/trust databases, loopback UDP and
// synthetic/discard Session workers. Never opens an audio device or invite file.
#include "Session.h"
#include "gui/audio/PairedAudioControl.h"
#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QMetaObject>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <thread>

namespace {
bool waitFor(const std::function<bool()> &predicate, int timeout)
{
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents();
    if (predicate())
      return true;
    QThread::msleep(2);
  } while (timer.elapsed() < timeout);
  return predicate();
}
bool trust(const QString &database, const QString &identity)
{
  QFile file(identity);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const QSslCertificate cert(file.readAll(), QSsl::Pem);
  if (cert.isNull())
    return false;
  FingerprintDatabase db;
  db.addTrusted({QCryptographicHash::Sha256, cert.digest(QCryptographicHash::Sha256)});
  return db.write(database);
}
QJsonObject offerJson(const preview::Invitation &offer)
{
  return {{"media", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(offer.mediaKey.data()), 30).toBase64())},
          {"control", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(offer.controlKey.data()), 32).toBase64())},
          {"ssrc", QString::number(offer.ssrc)}, {"expires", QString::number(offer.expiresUnixSeconds)},
          {"port", static_cast<int>(offer.port)}};
}
bool readOffer(const QJsonObject &object, const QString &tlsAddress, preview::Invitation &offer)
{
  const auto media = QByteArray::fromBase64(object.value("media").toString().toLatin1());
  const auto control = QByteArray::fromBase64(object.value("control").toString().toLatin1());
  bool ssrcOk = false, expiryOk = false, ipv4 = false;
  offer.ssrc = object.value("ssrc").toString().toUInt(&ssrcOk);
  offer.expiresUnixSeconds = object.value("expires").toString().toULongLong(&expiryOk);
  const int port = object.value("port").toInt();
  const auto ip = QHostAddress(tlsAddress).toIPv4Address(&ipv4);
  if (media.size() != 30 || control.size() != 32 || !ssrcOk || !offer.ssrc || !expiryOk ||
      offer.expiresUnixSeconds <= preview::unixSeconds() || port < 1 || port > 65535 || !ipv4 ||
      QHostAddress(ip) != QHostAddress(QHostAddress::LocalHost))
    return false;
  std::copy(media.begin(), media.end(), offer.mediaKey.begin());
  std::copy(control.begin(), control.end(), offer.controlKey.begin());
  offer.port = static_cast<std::uint16_t>(port);
  // Ignore all untrusted offered addresses: use the actual pinned TLS peer only.
  offer.addresses = {QHostAddress(ip).toString().toStdString()};
  return true;
}
struct Worker
{
  std::atomic<bool> stop{false}, done{false}, succeeded{false}, connected{false};
  std::atomic<std::uint64_t> packets{0};
  std::thread thread;
  ~Worker()
  {
    stop = true;
    if (thread.joinable())
      thread.join();
  }
  void start(preview::SessionOptions options)
  {
    thread = std::thread([this, options = std::move(options)]() mutable {
      try {
        preview::runSession(options, stop, [this](const preview::SessionEvent &event) {
          if (event.state == preview::SessionState::Connected) {
            connected = true;
            auto previous = packets.load();
            while (event.packets > previous && !packets.compare_exchange_weak(previous, event.packets)) {}
          }
        });
        succeeded = true;
      } catch (...) {
        // No exception payload or control credentials are logged.
      }
      if (options.invitation)
        preview::erase(*options.invitation);
      done = true;
    });
  }
};
} // namespace
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  QTemporaryDir temporary;
  if (!temporary.isValid())
    return 1;
  const auto receiverPem = temporary.filePath(QStringLiteral("original-server.pem"));
  const auto senderPem = temporary.filePath(QStringLiteral("original-client.pem"));
  const auto clientsDb = temporary.filePath(QStringLiteral("trusted-clients"));
  const auto serversDb = temporary.filePath(QStringLiteral("trusted-servers"));
  try {
    deskflow::generatePemSelfSignedCert(receiverPem, 2048);
    deskflow::generatePemSelfSignedCert(senderPem, 2048);
  } catch (...) {
    return 2;
  }
  // Establish the ORIGINAL pair trust once. No later code writes these databases.
  if (!trust(clientsDb, senderPem) || !trust(serversDb, receiverPem))
    return 3;
  QTcpServer probe;
  if (!probe.listen(QHostAddress::LocalHost, 0))
    return 4;
  const auto port = probe.serverPort();
  probe.close();
  PairedAudioControl receiverControl, senderControl;
  PairedAudioControl::Configuration receiverProfile, senderProfile;
  receiverProfile.useSettings = senderProfile.useSettings = false;
  receiverProfile.certificatePath = receiverPem;
  receiverProfile.trustedClientsPath = clientsDb;
  receiverProfile.localName = QStringLiteral("original-server");
  senderProfile.certificatePath = senderPem;
  senderProfile.trustedServersPath = serversDb;
  senderProfile.localName = QStringLiteral("original-client");
  receiverProfile.port = senderProfile.port = port;
  receiverControl.setConfiguration(receiverProfile);
  senderControl.setConfiguration(senderProfile);
  Worker receiver, sender; // Workers join before the control objects are destroyed.
  // Contextless test callbacks capture workers. Disconnect them before worker
  // destruction, because a controller destructor also emits peerDisconnected.
  struct DisconnectGuard
  {
    PairedAudioControl &first, &second;
    ~DisconnectGuard()
    {
      QObject::disconnect(&first, nullptr, nullptr, nullptr);
      QObject::disconnect(&second, nullptr, nullptr, nullptr);
    }
  } disconnectGuard{receiverControl, senderControl};
  const auto session = QUuid::createUuid().toString(QUuid::Id128);
  int offers = 0, answers = 0, stops = 0;
  bool invalid = false;
  QString receiverPeer, senderPeer;
  QObject::connect(&receiverControl, &PairedAudioControl::peerConnected,
                   [&](const QString &peer, const QString &) {
    if (receiver.thread.joinable()) {
      invalid = true;
      return;
    }
    receiverPeer = peer;
    preview::SessionOptions options;
    options.receive = true;
    options.test = true; // Decode/discard: no WASAPI render, no feedback.
    options.onReady = [&, peer](const preview::Invitation &value) {
      const auto payload = offerJson(value);
      QMetaObject::invokeMethod(&receiverControl, [&, peer, payload] {
        receiverControl.send(peer, {{"type", "offer"}, {"session", session}, {"offer", payload}});
      }, Qt::QueuedConnection);
    };
    receiver.start(std::move(options));
  });
  QObject::connect(&senderControl, &PairedAudioControl::messageReceived,
                   [&](const QString &peer, const QJsonObject &message) {
    if (message.value("type") == QStringLiteral("stop")) {
      if (peer == senderPeer && message.value("session").toString() == session)
        ++stops;
      else
        invalid = true;
      return;
    }
    if (message.value("type") != QStringLiteral("offer") || message.value("session").toString() != session ||
        sender.thread.joinable()) {
      invalid = true;
      return;
    }
    preview::Invitation offer;
    if (!readOffer(message.value("offer").toObject(), senderControl.peerAddress(peer), offer)) {
      invalid = true;
      preview::erase(offer);
      return;
    }
    senderPeer = peer;
    ++offers;
    senderControl.send(peer, {{"type", "answer"}, {"session", session}});
    preview::SessionOptions options;
    options.test = true; // Quiet synthetic tone, no capture device.
    options.invitation = offer;
    preview::erase(offer);
    sender.start(std::move(options));
  });
  QObject::connect(&receiverControl, &PairedAudioControl::messageReceived,
                   [&](const QString &peer, const QJsonObject &message) {
    if (peer == receiverPeer && message.value("type") == QStringLiteral("answer") &&
        message.value("session").toString() == session)
      ++answers;
    else
      invalid = true;
  });
  QObject::connect(&receiverControl, &PairedAudioControl::peerDisconnected, [&](const QString &) {
    receiver.stop = true;
    sender.stop = true;
  });
  QObject::connect(&senderControl, &PairedAudioControl::peerDisconnected, [&](const QString &) {
    receiver.stop = true;
    sender.stop = true;
  });
  receiverControl.configure(true, true, {}, {QStringLiteral("original-client")});
  senderControl.configure(false, true, QStringLiteral("127.0.0.1:24800"));
  if (!waitFor([&] { return invalid || (receiver.done && sender.done); }, 12000) || invalid)
    return 5;
  if (!receiver.succeeded || !sender.succeeded || !receiver.connected || !sender.connected ||
      receiver.packets < 600 || sender.packets < 600 || offers != 1 || answers != 1)
    return 6;
  receiverControl.send(receiverPeer, {{"type", "stop"}, {"session", session}});
  if (!waitFor([&] { return stops == 1; }, 1000))
    return 7;
  const auto files = QDir(temporary.path()).entryList(QDir::Files);
  if (files.size() != 4 || !QDir(temporary.path()).entryList({QStringLiteral("*.dshaudio")}, QDir::Files).isEmpty())
    return 8;
  // The original trust database remains the only trust store, unchanged.
  FingerprintDatabase clients, servers;
  clients.read(clientsDb);
  servers.read(serversDb);
  if (clients.fingerprints().size() != 1 || servers.fingerprints().size() != 1)
    return 9;
  std::cout << "original_pair_trust=pass mutual_tls_offer_answer_stop=pass memory_only=pass invitation_files=0 "
            << "synthetic_sent=" << sender.packets.load() << " srtp_opus_decoded=" << receiver.packets.load()
            << " real_audio_devices=0\n";
  return 0;
}
