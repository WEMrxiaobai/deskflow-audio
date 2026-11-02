/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "PairedAudioControl.h"

#include "common/Settings.h"
#include "net/FingerprintDatabase.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <utility>

namespace {
constexpr qsizetype kMaxFrame = 4096;
constexpr qint64 kMaxPendingWrite = 16384;
constexpr auto kMarker = "$deskflowAudio";
// Transport-owned frames are consumed internally, never delivered as messages.
// Both peers probe independently; a ping yields exactly one pong, never an echo.
constexpr auto kHeartbeat = "$deskflowHeartbeat";
constexpr qint64 kPingIntervalMs = 2000;
constexpr qint64 kPeerTimeoutMs = 10000;
class Listener final : public QTcpServer
{
public:
  std::function<void(qintptr)> acceptDescriptor;
  using QTcpServer::QTcpServer;

protected:
  void incomingConnection(qintptr descriptor) override
  {
    if (acceptDescriptor)
      acceptDescriptor(descriptor);
  }
};
QString hostOnly(const QString &input)
{
  const auto text = input.trimmed();
  if (text.startsWith('[')) {
    const auto end = text.indexOf(']');
    return end > 1 ? text.mid(1, end - 1) : QString();
  }
  QHostAddress numeric;
  if (numeric.setAddress(text))
    return text;
  if (text.contains(QStringLiteral("://")))
    return QUrl(text).host();
  if (text.count(':') == 1)
    return text.section(':', 0, 0);
  return text;
}
// Deskflow pins self-signed identities instead of using PKI/hostnames. Do not
// ignore expiry, revocation, weak signatures, missing certs, or arbitrary errors.
bool pinReplacesPki(QSslError::SslError error)
{
  return error == QSslError::SelfSignedCertificate || error == QSslError::SelfSignedCertificateInChain ||
         error == QSslError::CertificateUntrusted || error == QSslError::HostNameMismatch ||
         error == QSslError::UnableToGetLocalIssuerCertificate || error == QSslError::UnableToVerifyFirstCertificate;
}
} // namespace

struct PairedAudioControl::Impl
{
  struct Peer
  {
    QByteArray buffer;
    QString id;
    QString name;
    bool tlsVerified = false;
    bool ready = false;
    qint64 acceptedAt = 0;
    qint64 deadline = 0;
    qint64 lastSeen = 0;
    qint64 nextPing = 0;
  };
  PairedAudioControl *q;
  Configuration configuration;
  bool server = false;
  bool keyboardReady = false;
  bool active = false;
  QString host;
  QString serverFingerprint;
  QHash<QString, QString> identities;
  QString status;
  QTimer tick;
  QElapsedTimer clock; // All transport deadlines are monotonic, not wall time.
  Listener listener;
  QHash<QSslSocket *, Peer> sockets;
  QHash<QString, QSslSocket *> peers;
  QSslCertificate localCertificate;
  QSslKey localKey;
  QByteArray identity;
  QString localName;
  QString trustPath;
  qint64 nextConnect = 0;

  explicit Impl(PairedAudioControl *owner) : q(owner)
  {
    clock.start();
    listener.acceptDescriptor = [this](qintptr descriptor) {
      auto *socket = new QSslSocket(q);
      if (!socket->setSocketDescriptor(descriptor)) {
        socket->deleteLater();
        return;
      }
      if (!active || !server || !admit(socket)) {
        socket->abort();
        socket->deleteLater();
        return;
      }
      attach(socket);
      socket->startServerEncryption();
    };
    tick.setInterval(1000);
    QObject::connect(&tick, &QTimer::timeout, q, [this] { maintain(); });
    tick.start();
  }
  bool admit(QSslSocket *incoming)
  {
    // Keep incomplete handshakes bounded per address and globally. An older
    // unauthenticated attempt may yield to a newcomer, but never displace an
    // authenticated peer. A minimum age gives honest handshakes time to finish.
    constexpr int maxSockets = 8;
    constexpr int maxPendingPerAddress = 2;
    constexpr qint64 minimumHandshakeMs = 1000;
    const auto now = clock.elapsed();
    int sameAddress = 0;
    QSslSocket *oldestSame = nullptr;
    QSslSocket *oldest = nullptr;
    auto older = [this](QSslSocket *a, QSslSocket *b) {
      return !b || sockets.value(a).acceptedAt < sockets.value(b).acceptedAt;
    };
    for (auto it = sockets.cbegin(); it != sockets.cend(); ++it) {
      if (it->tlsVerified)
        continue;
      if (it.key()->peerAddress() == incoming->peerAddress()) {
        ++sameAddress;
        if (older(it.key(), oldestSame))
          oldestSame = it.key();
      }
      if (older(it.key(), oldest))
        oldest = it.key();
    }
    auto *victim = sameAddress >= maxPendingPerAddress ? oldestSame
                   : sockets.size() >= maxSockets ? oldest : nullptr;
    if (victim) {
      if (now - sockets.value(victim).acceptedAt < minimumHandshakeMs)
        return false;
      drop(victim);
    } else if (sameAddress >= maxPendingPerAddress || sockets.size() >= maxSockets) {
      return false;
    }
    return true;
  }
  void report(const QString &value)
  {
    if (status != value) {
      status = value;
      Q_EMIT q->statusChanged(status);
    }
  }
  void drop(QSslSocket *socket)
  {
    auto it = sockets.find(socket);
    if (it == sockets.end())
      return;
    const auto id = it->id;
    const bool notify = it->ready && peers.value(id) == socket;
    if (notify)
      peers.remove(id);
    sockets.erase(it);
    QObject::disconnect(socket, nullptr, q, nullptr);
    socket->abort();
    socket->deleteLater();
    if (notify)
      Q_EMIT q->peerDisconnected(id);
  }
  void stop()
  {
    active = false;
    listener.close();
    const auto all = sockets.keys();
    for (auto *socket : all)
      drop(socket);
    nextConnect = 0;
  }
  bool loadIdentity()
  {
    const bool settings = configuration.useSettings;
    const bool enabled = settings ? Settings::value(Settings::Security::TlsEnabled).toBool() : configuration.tlsEnabled;
    const bool check = settings ? Settings::value(Settings::Security::CheckPeers).toBool() : configuration.checkPeers;
    if (!keyboardReady || !enabled || (server && !check) || (!server && serverFingerprint.isEmpty()))
      return false;
    if (QSslSocket::activeBackend() != QStringLiteral("openssl") &&
        !QSslSocket::setActiveBackend(QStringLiteral("openssl")))
      return false;
    if (!QSslSocket::supportsSsl())
      return false;
    const auto certPath =
        settings ? Settings::value(Settings::Security::Certificate).toString() : configuration.certificatePath;
    const auto keyPath = settings || configuration.privateKeyPath.isEmpty() ? certPath : configuration.privateKeyPath;
    QFile certFile(certPath), keyFile(keyPath);
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly) || certFile.size() > 65536 ||
        keyFile.size() > 65536)
      return false;
    const auto certs = QSslCertificate::fromData(certFile.readAll(), QSsl::Pem);
    if (certs.isEmpty())
      return false;
    const auto cert = certs.first();
    const auto now = QDateTime::currentDateTimeUtc();
    if (cert.isNull() || cert.effectiveDate() > now || cert.expiryDate() <= now)
      return false;
    const auto keyData = keyFile.readAll();
    auto key = QSslKey(keyData, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull())
      key = QSslKey(keyData, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull())
      return false;
    const auto newIdentity = cert.digest(QCryptographicHash::Sha256);
    const auto newTrustPath = settings ? (server ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb())
                                       : (server ? configuration.trustedClientsPath : configuration.trustedServersPath);
    const auto newName = settings ? Settings::value(Settings::Core::ComputerName).toString() : configuration.localName;
    if (newName.isEmpty() || newName.size() > 255 || newTrustPath.isEmpty())
      return false;
    if (active && (identity != newIdentity || trustPath != newTrustPath || localName != newName || localKey != key))
      stop();
    localCertificate = cert;
    localKey = key;
    identity = newIdentity;
    trustPath = newTrustPath;
    localName = newName;
    return true;
  }
  bool trusted(QSslSocket *socket) const
  {
    const auto cert = socket->peerCertificate();
    const auto now = QDateTime::currentDateTimeUtc();
    if (cert.isNull() || cert.effectiveDate() > now || cert.expiryDate() <= now)
      return false;
    FingerprintDatabase db;
    db.read(trustPath);
    return db.isTrusted({QCryptographicHash::Sha256, cert.digest(QCryptographicHash::Sha256)});
  }
  void attach(QSslSocket *socket)
  {
    auto config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificate(localCertificate);
    config.setPrivateKey(localKey);
    config.setProtocol(QSsl::TlsV1_2OrLater);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    config.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
    config.setSslOption(QSsl::SslOptionDisableSessionSharing, true);
    config.setSslOption(QSsl::SslOptionDisableSessionPersistence, true);
    config.setSslOption(QSsl::SslOptionDisableCompression, true);
    socket->setSslConfiguration(config);
    socket->setReadBufferSize(kMaxFrame + 1);
    Peer peer;
    peer.acceptedAt = clock.elapsed();
    peer.deadline = peer.acceptedAt + 5000;
    sockets.insert(socket, peer);
    QObject::connect(socket, &QSslSocket::sslErrors, q, [this, socket](const QList<QSslError> &errors) {
      if (!sockets.contains(socket) || !trusted(socket)) {
        drop(socket);
        return;
      }
      for (const auto &error : errors) {
        if (!pinReplacesPki(error.error())) {
          drop(socket);
          return;
        }
      }
      socket->ignoreSslErrors(errors);
    });
    QObject::connect(socket, &QSslSocket::encrypted, q, [this, socket] {
      auto it = sockets.find(socket);
      if (it == sockets.end())
        return;
      if (!active || !trusted(socket) ||
          (socket->sessionProtocol() != QSsl::TlsV1_2 && socket->sessionProtocol() != QSsl::TlsV1_3)) {
        drop(socket);
        return;
      }
      it->tlsVerified = true;
      it->id = QString::fromLatin1(socket->peerCertificate().digest(QCryptographicHash::Sha256).toHex());
      if (!server && it->id != serverFingerprint) {
        drop(socket);
        return;
      }
      write(socket, {{QString::fromLatin1(kMarker), 1}, {QStringLiteral("name"), localName}});
    });
    QObject::connect(socket, &QSslSocket::readyRead, q, [this, socket] { read(socket); });
    QObject::connect(socket, &QSslSocket::disconnected, q, [this, socket] { drop(socket); });
    QObject::connect(socket, &QSslSocket::errorOccurred, q, [this, socket](QAbstractSocket::SocketError) {
      drop(socket);
    });
  }
  bool write(QSslSocket *socket, const QJsonObject &object)
  {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > kMaxFrame || socket->bytesToWrite() + bytes.size() > kMaxPendingWrite ||
        socket->write(bytes) != bytes.size()) {
      drop(socket);
      return false;
    }
    return true;
  }
  void read(QSslSocket *socket)
  {
    auto it = sockets.find(socket);
    if (it == sockets.end())
      return;
    if (!it->tlsVerified || !socket->isEncrypted() || !trusted(socket)) {
      drop(socket);
      return;
    }
    // Read bounded pieces; a newline is mandatory and each complete frame is
    // independently limited, including under arbitrary TCP fragmentation.
    while (socket->bytesAvailable() > 0) {
      it = sockets.find(socket);
      if (it == sockets.end())
        return;
      const auto room = kMaxFrame - it->buffer.size();
      if (room <= 0) {
        drop(socket);
        return;
      }
      it->buffer += socket->read(qMin<qint64>(room, socket->bytesAvailable()));
      while (true) {
        it = sockets.find(socket);
        if (it == sockets.end())
          return;
        const auto end = it->buffer.indexOf('\n');
        if (end < 0)
          break;
        const auto frame = it->buffer.left(end);
        it->buffer.remove(0, end + 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(frame, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
          drop(socket);
          return;
        }
        const auto message = document.object();
        if (!it->ready) {
          const auto nameValue = message.value(QStringLiteral("name"));
          const auto name = nameValue.toString();
          if (message.value(QString::fromLatin1(kMarker)).toInt(-1) != 1 || !nameValue.isString() || name.isEmpty() ||
              name.size() > 255 || message.contains(QString::fromLatin1(kHeartbeat)) ||
              (server && identities.value(name) != it->id)) {
            drop(socket);
            return;
          }
          const auto id = it->id;
          // Only a fully authenticated protocol-speaking socket replaces a peer.
          if (auto *previous = peers.value(id); previous && previous != socket)
            drop(previous);
          it = sockets.find(socket);
          if (it == sockets.end())
            return;
          it->name = name;
          it->ready = true;
          it->lastSeen = clock.elapsed();
          it->nextPing = it->lastSeen + kPingIntervalMs;
          peers.insert(id, socket);
          Q_EMIT q->peerConnected(id, name);
        } else {
          if (message.contains(QString::fromLatin1(kMarker))) {
            drop(socket);
            return;
          }
          if (message.contains(QString::fromLatin1(kHeartbeat))) {
            const auto value = message.value(QString::fromLatin1(kHeartbeat));
            if (message.size() != 1 || !value.isString() ||
                (value.toString() != QStringLiteral("ping") && value.toString() != QStringLiteral("pong"))) {
              drop(socket);
              return;
            }
            it->lastSeen = clock.elapsed();
            if (value.toString() == QStringLiteral("ping") &&
                !write(socket, {{QString::fromLatin1(kHeartbeat), QStringLiteral("pong")}}))
              return;
            continue;
          }
          // Only complete authenticated frames refresh liveness; partial TCP
          // bytes cannot keep a wedged/slow-drip connection alive indefinitely.
          it->lastSeen = clock.elapsed();
          const auto id = it->id;
          Q_EMIT q->messageReceived(id, message);
        }
      }
    }
  }
  void maintain()
  {
    if (!loadIdentity()) {
      stop();
      report(QStringLiteral("音频等待已验证的键鼠连接、原证书和设备信任"));
      return;
    }
    active = true;
    const auto now = clock.elapsed();
    const auto all = sockets.keys();
    for (auto *socket : all) {
      const auto it = sockets.constFind(socket);
      if (it == sockets.cend())
        continue;
      if ((!it->ready && now >= it->deadline) || (it->tlsVerified && !trusted(socket)) ||
          (it->ready && now - it->lastSeen >= kPeerTimeoutMs) || (server && it->ready && identities.value(it->name) != it->id)) {
        drop(socket);
        continue;
      }
      if (it->ready && now >= it->nextPing) {
        // Advance before write: dropping an overfull socket invalidates it.
        sockets[socket].nextPing = now + kPingIntervalMs;
        write(socket, {{QString::fromLatin1(kHeartbeat), QStringLiteral("ping")}});
      }
    }
    if (server) {
      if (!listener.isListening() && !listener.listen(QHostAddress::Any, configuration.port)) {
        report(QStringLiteral("音频控制端口不可用；键鼠连接不受影响"));
        return;
      }
      report(QStringLiteral("已使用原设备身份等待音频连接"));
    } else if (sockets.isEmpty() && !host.isEmpty() && now >= nextConnect) {
      nextConnect = now + 2000;
      auto *socket = new QSslSocket(q);
      attach(socket);
      socket->connectToHostEncrypted(host, configuration.port);
      report(QStringLiteral("正在通过原设备信任连接音频"));
    }
  }
};

PairedAudioControl::PairedAudioControl(QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this))
{
}
PairedAudioControl::~PairedAudioControl()
{
  d->tick.stop();
  d->stop();
}
void PairedAudioControl::setConfiguration(const Configuration &configuration)
{
  d->stop();
  d->configuration = configuration;
  d->maintain();
}
void PairedAudioControl::configure(
    bool server, bool keyboardReady, const QString &serverHost, const QHash<QString, QString> &connectedIdentities,
    const QString &serverFingerprint
)
{
  const auto host = hostOnly(serverHost);
  if (d->server != server || d->keyboardReady != keyboardReady || d->host != host ||
      (!server && d->serverFingerprint != serverFingerprint))
    d->stop();
  d->server = server;
  d->keyboardReady = keyboardReady;
  d->host = host;
  d->serverFingerprint = serverFingerprint;
  d->identities = connectedIdentities;
  d->maintain();
}
void PairedAudioControl::send(const QString &id, const QJsonObject &message)
{
  auto *socket = d->peers.value(id);
  if (!socket || message.contains(QString::fromLatin1(kMarker)) || message.contains(QString::fromLatin1(kHeartbeat)))
    return;
  if (!d->active || !socket->isEncrypted() || !d->trusted(socket)) {
    d->drop(socket);
    return;
  }
  d->write(socket, message);
}
QStringList PairedAudioControl::peerIds() const
{
  return d->peers.keys();
}
QString PairedAudioControl::peerAddress(const QString &id) const
{
  const auto *socket = d->peers.value(id);
  return socket ? socket->peerAddress().toString() : QString();
}
bool PairedAudioControl::isServer() const
{
  return d->server;
}
QString PairedAudioControl::localAddress(const QString &id) const
{
  const auto *socket = d->peers.value(id);
  return socket ? socket->localAddress().toString() : QString();
}
