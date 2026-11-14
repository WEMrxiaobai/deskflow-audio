/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Isolated transport/security tests: no installed settings, real audio, core,
// service, or trust changes. Pass the openssl executable as the first argument.
#include "Session.h"
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
#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <thread>

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
  process.start(
      openssl, {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
                QStringLiteral("-nodes"), QStringLiteral("-days"), QStringLiteral("1"), QStringLiteral("-subj"),
                QStringLiteral("/CN=") + name, QStringLiteral("-keyout"), pem, QStringLiteral("-out"),
                pem + QStringLiteral(".crt")}
  );
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

// 两个 Session worker 使用合成声音与回环 UDP；只有恢复请求/位置经真实的原身份 mTLS。
// 故意阻塞接收 worker 120ms，验证恢复无需重新发布邀请，不打开声卡或新增信任记录。
bool recoverMedia(PairedAudioControl &receiverControl, PairedAudioControl &senderControl)
{
  auto receiverInbox = std::make_shared<preview::SessionRecovery>();
  auto senderInbox = std::make_shared<preview::SessionRecovery>();
  const auto receiverPeer = receiverControl.peerIds().first();
  const auto senderPeer = senderControl.peerIds().first();
  const auto session = QStringLiteral("00112233445566778899aabbccddeeff");
  std::atomic<bool> receiverStop{false}, senderStop{false}, receiverDone{false}, senderDone{false};
  std::atomic<bool> succeeded{true};
  std::atomic<int> recovering{0}, recovered{0};
  int requests = 0, responses = 0;
  auto sendRecovery = [&](PairedAudioControl &control, const QString &peer, const preview::RecoveryMessage &value) {
    const QJsonObject message{{"type", value.response ? "resume" : "recover"}, {"session", session},
                              {"request", QString::number(value.request)},     {"sequence", value.sequence},
                              {"timestamp", QString::number(value.timestamp)}, {"hasMedia", value.hasMedia}};
    QMetaObject::invokeMethod(
        &control, [&control, peer, message] { control.send(peer, message); }, Qt::QueuedConnection
    );
  };
  const auto requestConnection = QObject::connect(
      &senderControl, &PairedAudioControl::messageReceived,
      [&](const QString &peer, const QJsonObject &message) {
        if (peer != senderPeer || message.value("session").toString() != session || message.value("type") != "recover")
          return;
        ++requests;
        senderInbox->post({false, message.value("request").toString().toULongLong()});
      }
  );
  const auto responseConnection = QObject::connect(
      &receiverControl, &PairedAudioControl::messageReceived,
      [&](const QString &peer, const QJsonObject &message) {
        if (peer != receiverPeer || message.value("session").toString() != session || message.value("type") != "resume")
          return;
        ++responses;
        receiverInbox->post(
            {true, message.value("request").toString().toULongLong(),
             static_cast<std::uint16_t>(message.value("sequence").toInt()),
             message.value("timestamp").toString().toUInt(), message.value("hasMedia").toBool(true)}
        );
      }
  );
  std::promise<preview::Invitation> ready;
  auto offer = ready.get_future();
  preview::SessionOptions receiver;
  receiver.receive = receiver.test = true;
  receiver.recovery = receiverInbox;
  receiver.onRecovery = [&](const auto &message) { sendRecovery(receiverControl, receiverPeer, message); };
  receiver.onReady = [&](const auto &invitation) { ready.set_value(invitation); };
  std::thread receiveWorker([&] {
    bool paused = false;
    try {
      preview::runSession(receiver, receiverStop, [&](const auto &event) {
        if (event.state == preview::SessionState::Recovering)
          ++recovering;
        if (event.state == preview::SessionState::Recovered)
          ++recovered;
        if (!paused && event.state == preview::SessionState::Connected && event.packets > 50) {
          paused = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
      });
    } catch (...) {
      succeeded = false;
    }
    receiverDone = true;
  });
  std::thread sendWorker;
  if (offer.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
    auto invitation = offer.get();
    preview::SessionOptions sender;
    sender.test = true;
    sender.invitation = invitation;
    preview::erase(invitation);
    sender.recovery = senderInbox;
    sender.onRecovery = [&](const auto &message) { sendRecovery(senderControl, senderPeer, message); };
    sendWorker = std::thread([&, sender = std::move(sender)]() mutable {
      try {
        preview::runSession(sender, senderStop);
      } catch (...) {
        succeeded = false;
      }
      preview::erase(*sender.invitation);
      senderDone = true;
    });
  } else {
    succeeded = false;
  }
  const bool completed = waitFor([&] { return receiverDone && senderDone; }, 9000);
  receiverStop = senderStop = true;
  if (sendWorker.joinable())
    sendWorker.join();
  receiveWorker.join();
  // drain 已入队的回调，再解除捕获栈变量的连接，避免测试退出后的悬空访问。
  QCoreApplication::processEvents();
  QObject::disconnect(requestConnection);
  QObject::disconnect(responseConnection);
  return completed && succeeded && recovering >= 1 && recovered == recovering && requests >= 1 && requests == responses;
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
  const auto aliceDb = directory.filePath(QStringLiteral("alice-trusted-clients"));
  const auto bobDb = directory.filePath(QStringLiteral("bob-trusted-servers"));
  if (!generate(QString::fromLocal8Bit(argv[1]), alicePem, QStringLiteral("alice")) ||
      !generate(QString::fromLocal8Bit(argv[1]), bobPem, QStringLiteral("bob")) || !trust(aliceDb, bobPem) ||
      !trust(bobDb, alicePem))
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
  // Local name is only a liveness restriction: old trust alone is insufficient.
  alice.configure(true, true, {}, {QStringLiteral("not-bob")});
  bob.configure(false, true, host);
  if (waitFor([&] { return !alice.peerIds().isEmpty(); }, 2500))
    return 5;
  alice.configure(true, true, {}, {QStringLiteral("bob")});
  if (!waitFor([&] { return alice.peerIds().size() == 1 && bob.peerIds().size() == 1; }))
    return 6;
  if (alice.peerIds().first() != QString::fromLatin1(fingerprint(bobPem).data.toHex()) ||
      bob.peerIds().first() != QString::fromLatin1(fingerprint(alicePem).data.toHex()) ||
      alice.peerAddress(alice.peerIds().first()).isEmpty())
    return 7;
  bob.send(bob.peerIds().first(), {{QStringLiteral("test"), QStringLiteral("confidential")}});
  if (!waitFor([&] { return received == 1; }))
    return 8;
  if (!recoverMedia(alice, bob))
    return 20;
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
  alice.configure(true, false, {}, {QStringLiteral("bob")});
  if (!alice.peerIds().isEmpty() || !waitFor([&] { return bob.peerIds().isEmpty(); }))
    return 18;
  // TLS without original client-certificate checking must never enable listening.
  a.checkPeers = false;
  alice.setConfiguration(a);
  alice.configure(true, true, {}, {QStringLiteral("bob")});
  if (waitFor([&] { return !alice.peerIds().isEmpty(); }, 2500))
    return 19;
  bob.configure(false, false, host);
  std::cout << "mutual_tls=pass identity_pin=pass original_name_gate=pass plaintext=reject "
               "bounded_frame=pass revocation=pass reconnect=pass keyboard_gate=pass unchecked_tls=reject "
               "tls_media_recovery=pass real_audio_devices=0\n";
  return 0;
}
