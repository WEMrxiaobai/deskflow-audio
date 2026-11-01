/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Explicit REAL-output test: actual widget/controller receive negotiation, quiet
// synthetic tone for five seconds. Never captures system audio or creates invites.
#include "Session.h"
#include "common/Constants.h"
#include "common/Settings.h"
#include "gui/audio/PairedAudioControl.h"
#include "gui/widgets/AudioSharingWidget.h"
#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QHostAddress>
#include <QLabel>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
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
class DialogWatch : public QObject
{
public:
  bool seen = false;
  bool eventFilter(QObject *object, QEvent *event) override
  {
    if (event->type() == QEvent::Show && (qobject_cast<QFileDialog *>(object) || qobject_cast<QMessageBox *>(object))) {
      seen = true;
      if (auto *dialog = qobject_cast<QDialog *>(object))
        QTimer::singleShot(0, dialog, &QDialog::reject);
    }
    return false;
  }
};
bool trust(const QString &path, const QString &pem)
{
  QFile file(pem);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const QSslCertificate cert(file.readAll(), QSsl::Pem);
  if (cert.isNull())
    return false;
  FingerprintDatabase database;
  database.addTrusted({QCryptographicHash::Sha256, cert.digest(QCryptographicHash::Sha256)});
  return database.write(path);
}
QByteArray contents(const QString &path)
{
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "Cannot read isolated trust fixture");
  return file.readAll();
}
struct Fixture
{
  QString path;
  bool settingsLoaded = false;
  ~Fixture()
  {
    if (settingsLoaded)
      Settings::save();
    // Only set after this test's atomic NewOnly creation succeeds.
    if (!path.isEmpty())
      QFile::remove(path);
  }
};
struct Sender
{
  std::atomic<bool> stop{false}, done{false}, succeeded{false}, connected{false};
  std::atomic<std::uint64_t> packets{0};
  std::thread worker;
  ~Sender()
  {
    join();
  }
  void join()
  {
    stop = true;
    if (worker.joinable())
      worker.join();
  }
  void start(const std::shared_ptr<preview::SessionOptions> &options)
  {
    worker = std::thread([this, options] {
      try {
        preview::runSession(*options, stop, [this](const preview::SessionEvent &event) {
          if (event.state == preview::SessionState::Connected) {
            connected = true;
            packets = std::max(packets.load(), event.packets);
          }
        });
        succeeded = true;
      } catch (...) {
        // Never log control bodies, keys, or exception payloads.
      }
      done = true;
    });
  }
};
bool decodeOffer(const QJsonObject &object, const QString &actualTlsAddress, preview::Invitation &offer)
{
  const auto media = QByteArray::fromBase64(object.value("media").toString().toLatin1());
  const auto control = QByteArray::fromBase64(object.value("control").toString().toLatin1());
  bool ssrcOk = false, expiryOk = false, ipv4 = false;
  offer.ssrc = object.value("ssrc").toString().toUInt(&ssrcOk);
  offer.expiresUnixSeconds = object.value("expires").toString().toULongLong(&expiryOk);
  const auto port = object.value("port").toInt();
  const auto ip = QHostAddress(actualTlsAddress).toIPv4Address(&ipv4);
  const auto now = preview::unixSeconds();
  if (media.size() != 30 || control.size() != 32 || !ssrcOk || !offer.ssrc || !expiryOk ||
      offer.expiresUnixSeconds <= now || offer.expiresUnixSeconds > now + 600 || port < 1 || port > 65535 || !ipv4 ||
      QHostAddress(ip) != QHostAddress(QHostAddress::LocalHost))
    return false;
  std::copy(media.begin(), media.end(), offer.mediaKey.begin());
  std::copy(control.begin(), control.end(), offer.controlKey.begin());
  offer.port = static_cast<std::uint16_t>(port);
  offer.addresses = {QHostAddress(ip).toString().toStdString()};
  return true;
}
void exercise(QApplication &app, const QTemporaryDir &temporary, const QString &portableFolder)
{
  const auto serverPem = temporary.filePath(QStringLiteral("original-server.pem"));
  const auto clientPem = temporary.filePath(QStringLiteral("original-client.pem"));
  const auto clientsDb = temporary.filePath(QStringLiteral("trusted-clients"));
  const auto serversDb = temporary.filePath(QStringLiteral("trusted-servers"));
  deskflow::generatePemSelfSignedCert(serverPem, 2048);
  deskflow::generatePemSelfSignedCert(clientPem, 2048);
  check(trust(clientsDb, clientPem) && trust(serversDb, serverPem), "Cannot establish isolated original pair trust");
  const auto originalClients = contents(clientsDb), originalServers = contents(serversDb);
  const auto filesBefore = QDir(temporary.path()).entryList(QDir::Files);
  const auto portableBefore = QDir(portableFolder).entryList(QDir::Files);
  QTcpServer probe;
  check(probe.listen(QHostAddress::LocalHost, 0), "Cannot reserve control test port");
  const auto port = probe.serverPort();
  probe.close();
  AudioSharingWidget widget;
  // Intentionally no attach(CoreProcess): explicit authenticated test profiles
  // provide the already-connected keyboard identities without launching a core.
  auto *control = widget.findChild<PairedAudioControl *>();
  auto *listen = widget.findChild<QCheckBox *>(QStringLiteral("audioListenSwitch"));
  auto *status = widget.findChild<QLabel *>(QStringLiteral("audioStatusLabel"));
  auto *stop = widget.findChild<QPushButton *>(QStringLiteral("audioStopButton"));
  check(control && listen && status && stop && !listen->isChecked(), "Missing production widget controls");
  PairedAudioControl remote;
  Sender sender;
  QObject callbacks;
  DialogWatch dialogs;
  app.installEventFilter(&dialogs);
  QString peer, activeSession;
  int offers = 0, stops = 0, disconnected = 0;
  bool invalid = false, listeningSeen = false, switchOffAdvertised = false;
  // All callback captures remain alive until this guard disconnects. Also stop
  // both workers/controllers on every assertion/exception path, not only success.
  struct Cleanup
  {
    QApplication &app;
    DialogWatch &dialogs;
    AudioSharingWidget &widget;
    QCheckBox *listen;
    PairedAudioControl &control, &remote;
    QObject &callbacks;
    Sender &sender;
    ~Cleanup()
    {
      QObject::disconnect(&control, nullptr, &callbacks, nullptr);
      QObject::disconnect(&remote, nullptr, &callbacks, nullptr);
      if (listen->isChecked())
        listen->click();
      sender.join();
      control.configure(false, false, {});
      remote.configure(false, false, {});
      app.removeEventFilter(&dialogs);
    }
  } cleanup{app, dialogs, widget, listen, *control, remote, callbacks, sender};
  PairedAudioControl::Configuration localProfile, remoteProfile;
  localProfile.useSettings = remoteProfile.useSettings = false;
  localProfile.certificatePath = serverPem;
  localProfile.trustedClientsPath = clientsDb;
  localProfile.localName = QStringLiteral("original-server");
  remoteProfile.certificatePath = clientPem;
  remoteProfile.trustedServersPath = serversDb;
  remoteProfile.localName = QStringLiteral("original-client");
  localProfile.port = remoteProfile.port = port;
  control->setConfiguration(localProfile);
  remote.setConfiguration(remoteProfile);
  QObject::connect(&remote, &PairedAudioControl::peerConnected, &callbacks, [&](const QString &id, const QString &) {
    peer = id;
    remote.send(id, {{"type", "state"}, {"listen", false}, {"available", true}});
  });
  QObject::connect(control, &PairedAudioControl::peerDisconnected, &callbacks, [&](const QString &) {
    ++disconnected;
  });
  QObject::connect(&remote, &PairedAudioControl::peerDisconnected, &callbacks, [&](const QString &) {
    ++disconnected;
  });
  QObject::connect(
      &remote, &PairedAudioControl::messageReceived, &callbacks,
      [&](const QString &id, const QJsonObject &message) {
        const auto type = message.value("type").toString();
        if (type == "state") {
          switchOffAdvertised |= offers > 0 && message.value("listen").isBool() && !message.value("listen").toBool();
          return;
        }
        if (type == "stop") {
          if (id == peer && message.value("session").toString() == activeSession)
            ++stops;
          else
            invalid = true;
          return;
        }
        if (type != "offer" || id != peer || sender.worker.joinable()) {
          invalid = true;
          return;
        }
        activeSession = message.value("session").toString();
        auto options = std::shared_ptr<preview::SessionOptions>(new preview::SessionOptions, [](auto *value) {
          if (value->invitation)
            preview::erase(*value->invitation);
          delete value;
        });
        options->test = true; // Synthetic .01 peak; NEVER loopback capture.
        options->invitation.emplace();
        if (activeSession.size() != 32 ||
            !decodeOffer(message.value("offer").toObject(), remote.peerAddress(id), *options->invitation)) {
          invalid = true;
          return;
        }
        ++offers;
        try {
          sender.start(options); // Sender onReady intentionally unset.
        } catch (...) {
          invalid = true;
        }
      }
  );
  widget.show();
  listen->click(); // Actual production switch drives offer creation and routing.
  check(listen->isChecked() && Settings::value(Settings::Gui::AudioListen).toBool(), "Widget switch did not enable");
  control->configure(true, true, {}, {QStringLiteral("original-client")});
  remote.configure(false, true, QStringLiteral("127.0.0.1"));
  check(
      waitFor(
          [&] {
            listeningSeen |= status->text().contains(QStringLiteral("正在收听已配对电脑的声音"));
            return invalid || sender.done.load();
          },
          15000
      ),
      "Actual widget media negotiation timed out"
  );
  check(
      !invalid && sender.succeeded && sender.connected && sender.packets >= 600 && offers == 1 && listeningSeen,
      "Actual widget failed authenticated offer / SRTP / render status"
  );
  listen->click(); // Must stop/reconcile media WITHOUT dropping the original TLS channel.
  check(!listen->isChecked() && !Settings::value(Settings::Gui::AudioListen).toBool(), "Widget switch did not stop");
  check(
      waitFor([&] { return switchOffAdvertised && stops > 0; }, 1500), "Switch-off control state/stop was not delivered"
  );
  check(
      disconnected == 0 && control->peerIds().size() == 1 && remote.peerIds().size() == 1,
      "Stopping widget media dropped its paired TLS control connection"
  );
  check(
      !dialogs.seen && !QApplication::activeModalWidget() && stop->isHidden(),
      "Widget requested independent authorization"
  );
  check(
      QDir(temporary.path()).entryList(QDir::Files) == filesBefore &&
          QDir(portableFolder).entryList(QDir::Files) == portableBefore,
      "Widget created an invitation or unexpected persistent file"
  );
  check(
      contents(clientsDb) == originalClients && contents(serversDb) == originalServers,
      "Widget changed original pairing trust"
  );
  std::cout << "actual_widget_offer=1 quiet_synthetic_sent=" << sender.packets.load()
            << " real_render_status=" << listeningSeen
            << " switch_stop_tls_survives=1 dialogs=0 invitation_files=0 system_capture=0\n";
}
} // namespace
int main(int argc, char **argv)
{
  std::cout
      << "Explicit device test: five seconds of quiet synthetic audio through default output; no system capture.\n";
  QApplication::setApplicationName(kAppName);
  QApplication::setOrganizationName(QStringLiteral("DeskflowIntegrationTest"));
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  QTemporaryDir temporary;
  if (!temporary.isValid())
    return 1;
  qputenv("XDG_STATE_HOME", temporary.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
  const auto folder = QApplication::applicationDirPath() + QStringLiteral("/settings");
  if (!QDir().mkpath(folder))
    return 2;
  QLockFile lock(folder + QStringLiteral("/audio-smoke.lock"));
  if (!lock.tryLock(10000))
    return 3;
  Fixture fixture;
  QFile config(folder + QStringLiteral("/") + kAppName + QStringLiteral(".conf"));
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    std::cerr << "Refusing to overwrite existing portable configuration\n";
    return 4;
  }
  fixture.path = config.fileName();
  config.write(
      "[gui]\naudioListen=false\nstartCoreWithGui=false\nenableUpdateCheck=false\n"
      "[security]\ntlsEnabled=false\n[core]\nprocessMode=1\ncomputerName=widget-route-smoke\n"
  );
  config.close();
  try {
    fixture.settingsLoaded = true;
    check(Settings::settingsFile() == fixture.path, "Settings escaped isolated portable fixture");
    exercise(app, temporary, folder);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Paired audio widget test failed: " << error.what() << '\n';
    return 5;
  }
}
