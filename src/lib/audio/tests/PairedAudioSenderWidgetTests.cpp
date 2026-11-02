/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// EXPLICIT device integration test: renders a quiet .01 tone, captures system
// loopback through the REAL widget sender, and decodes/discards remotely. Never
// opens a microphone. Preserves an initially-muted output; otherwise verifies
// temporary mute/unmute. Counts only this test process's watchdog children.
#include "Session.h"
#include "audio/windows/WasapiBackend.h"
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
#include <QMetaObject>
#include <QPushButton>
#include <QPixmap>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>
#include <tlhelp32.h>

void failSenderWidgetReceive();

namespace {
using deskflow::audio::windows::WasapiBackend;
void check(bool condition, const char *message)
{
  if (!condition) throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()> &predicate, int milliseconds)
{
  QElapsedTimer timer; timer.start();
  do {
    QApplication::processEvents();
    if (predicate()) return true;
    QThread::msleep(2);
  } while (timer.elapsed() < milliseconds);
  return predicate();
}
bool trust(const QString &path, const QString &pem)
{
  QFile file(pem);
  if (!file.open(QIODevice::ReadOnly)) return false;
  const QSslCertificate certificate(file.readAll(), QSsl::Pem);
  if (certificate.isNull()) return false;
  FingerprintDatabase database;
  database.addTrusted({QCryptographicHash::Sha256, certificate.digest(QCryptographicHash::Sha256)});
  return database.write(path);
}
QByteArray contents(const QString &path)
{
  QFile file(path); check(file.open(QIODevice::ReadOnly), "Cannot read isolated fixture");
  return file.readAll();
}
QJsonObject offerJson(const preview::Invitation &offer)
{
  return {{"media", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(offer.mediaKey.data()), 30).toBase64())},
          {"control", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(offer.controlKey.data()), 32).toBase64())},
          {"ssrc", QString::number(offer.ssrc)}, {"expires", QString::number(offer.expiresUnixSeconds)},
          {"port", static_cast<int>(offer.port)}};
}
bool muteIs(bool expected)
{
  float volume = 0; bool muted = false;
  return WasapiBackend::readDefaultMasterVolume(volume, muted) >= 0 && muted == expected;
}
unsigned watchdogChildren()
{
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  check(snapshot != INVALID_HANDLE_VALUE, "Cannot inspect isolated watchdog lifetime");
  PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
  unsigned count = 0;
  if (Process32FirstW(snapshot, &entry)) do {
    if (entry.th32ParentProcessID == GetCurrentProcessId() &&
        _wcsicmp(entry.szExeFile, L"deskflow-audio-mute-watchdog.exe") == 0)
      ++count;
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return count;
}
struct Tone
{
  double phase = 0;
  std::atomic<bool> running{false};
  std::atomic<unsigned> callbacks{0};
  static void render(void *context, std::span<float> output,
                     const deskflow::audio::windows::DeviceFormat &format) noexcept
  {
    auto &self = *static_cast<Tone *>(context);
    if (!format.channels || !format.sampleRate) return;
    constexpr double tau = 6.2831853071795864769;
    for (std::size_t offset = 0; offset < output.size(); offset += format.channels) {
      const float sample = static_cast<float>(.01 * std::sin(self.phase));
      self.phase += tau * 440 / format.sampleRate;
      if (self.phase >= tau) self.phase -= tau;
      for (std::size_t channel = 0; channel < format.channels && offset + channel < output.size(); ++channel)
        output[offset + channel] = sample;
    }
    ++self.callbacks;
  }
  static void status(void *context, const deskflow::audio::windows::Status &status) noexcept
  {
    if (status.state == deskflow::audio::windows::State::Running)
      static_cast<Tone *>(context)->running = true;
  }
};
struct Receiver
{
  std::atomic<bool> stop{false}, done{false}, succeeded{false}, connected{false};
  std::atomic<std::uint64_t> packets{0};
  std::atomic<float> peak{0};
  std::thread worker;
  ~Receiver() { join(); }
  void join() { stop = true; if (worker.joinable()) worker.join(); }
  void start(preview::SessionOptions options)
  {
    join();
    stop = false; done = false; succeeded = false; connected = false; packets = 0; peak = 0;
    worker = std::thread([this, options = std::move(options)] {
      try {
        preview::runSession(options, stop, [this](const preview::SessionEvent &event) {
          if (event.state == preview::SessionState::Connected) connected = true;
          packets = std::max(packets.load(), event.packets);
          const std::string marker = "test_decoded_peak=";
          const auto position = event.message.find(marker);
          if (position != std::string::npos)
            peak = std::stof(event.message.substr(position + marker.size()));
        });
        succeeded = true;
      } catch (...) { /* Never log secret/exception payloads. */ }
      done = true;
    });
  }
};
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
void exercise(QApplication &app, const QTemporaryDir &temporary, bool remoteStop, bool originallyMuted)
{
  const auto serverPem = temporary.filePath(QStringLiteral("server.pem"));
  const auto clientPem = temporary.filePath(QStringLiteral("client.pem"));
  const auto clientsDb = temporary.filePath(QStringLiteral("clients"));
  const auto serversDb = temporary.filePath(QStringLiteral("servers"));
  const auto clientsBefore = contents(clientsDb), serversBefore = contents(serversDb);
  const auto filesBefore = QDir(temporary.path()).entryList(QDir::Files);
  QTcpServer probe; check(probe.listen(QHostAddress::LocalHost, 0), "Cannot reserve control port");
  const auto port = probe.serverPort(); probe.close();
  Settings::setValue(Settings::Gui::AudioLocalPlayback, false);
  Settings::setValue(Settings::Gui::AudioListen, false);
  AudioSharingWidget widget;
  auto *control = widget.findChild<PairedAudioControl *>();
  auto *local = widget.findChild<QCheckBox *>(QStringLiteral("audioLocalPlaybackSwitch"));
  auto *listen = widget.findChild<QCheckBox *>(QStringLiteral("audioListenSwitch"));
  auto *stop = widget.findChild<QPushButton *>(QStringLiteral("audioStopButton"));
  check(control && local && listen && stop && !local->isChecked() && !listen->isChecked(), "Bad default widget policy");
  PairedAudioControl remote;
  Receiver receiver;
  QObject callbacks;
  DialogWatch dialogs;
  app.installEventFilter(&dialogs);
  QString peer;
  auto session = QUuid::createUuid().toString(QUuid::Id128);
  int disconnected = 0, stopMessages = 0;
  struct Cleanup
  {
    QApplication &app; DialogWatch &dialogs; PairedAudioControl &control, &remote;
    QObject &callbacks; Receiver &receiver;
    ~Cleanup()
    {
      QObject::disconnect(&control, nullptr, &callbacks, nullptr);
      QObject::disconnect(&remote, nullptr, &callbacks, nullptr);
      control.configure(false, false, {}); remote.configure(false, false, {});
      receiver.join(); app.removeEventFilter(&dialogs);
    }
  } cleanup{app, dialogs, *control, remote, callbacks, receiver};
  PairedAudioControl::Configuration localProfile, remoteProfile;
  localProfile.useSettings = remoteProfile.useSettings = false;
  localProfile.certificatePath = serverPem; localProfile.trustedClientsPath = clientsDb;
  localProfile.localName = QStringLiteral("original-server");
  remoteProfile.certificatePath = clientPem; remoteProfile.trustedServersPath = serversDb;
  remoteProfile.localName = QStringLiteral("original-client");
  localProfile.port = remoteProfile.port = port;
  control->setConfiguration(localProfile); remote.setConfiguration(remoteProfile);
  QObject::connect(control, &PairedAudioControl::messageReceived, &callbacks, [&](const QString &, const QJsonObject &msg) {
    std::cerr << "received_type=" << msg.value("type").toString().toStdString() << '\n';
  });
  auto *diagnosticStatus = widget.findChild<QLabel *>(QStringLiteral("audioStatusLabel"));
  QTimer diagnosticTimer;
  QString previousStatus;
  QObject::connect(&diagnosticTimer, &QTimer::timeout, &callbacks, [&] {
    if (diagnosticStatus && previousStatus != diagnosticStatus->text()) {
      previousStatus = diagnosticStatus->text(); std::cerr << "status=" << previousStatus.toStdString() << '\n';
    }
  });
  diagnosticTimer.start(100);
  QObject::connect(control, &PairedAudioControl::peerDisconnected, &callbacks, [&](const QString &) { ++disconnected; });
  QObject::connect(&remote, &PairedAudioControl::peerDisconnected, &callbacks, [&](const QString &) { ++disconnected; });
  QObject::connect(&remote, &PairedAudioControl::messageReceived, &callbacks,
                   [&](const QString &, const QJsonObject &message) {
    if (message.value("type") == "stop" && message.value("session").toString() == session) ++stopMessages;
  });
  QObject::connect(&remote, &PairedAudioControl::peerConnected, &callbacks,
                   [&](const QString &id, const QString &) {
    peer = id;
    remote.send(id, {{"type", "state"}, {"listen", true}, {"available", true}});
    preview::SessionOptions options; options.receive = true; options.test = true; options.controlOwnsLifetime = true;
    options.onReady = [&, id](const preview::Invitation &offer) {
      const auto payload = offerJson(offer);
      QMetaObject::invokeMethod(&callbacks, [&, id, payload] {
        remote.send(id, {{"type", "offer"}, {"session", session}, {"offer", payload}});
      }, Qt::QueuedConnection);
    };
    receiver.start(std::move(options));
  });
  widget.show();
  control->configure(true, true, {}, {{QStringLiteral("original-client"), QString::fromLatin1(QSslCertificate::fromPath(clientPem).first().digest(QCryptographicHash::Sha256).toHex())}});
  remote.configure(false, true, QStringLiteral("127.0.0.1"), {}, QString::fromLatin1(QSslCertificate::fromPath(serverPem).first().digest(QCryptographicHash::Sha256).toHex()));
  const bool senderReady = waitFor([&] { return receiver.connected && muteIs(true) && local->isVisible(); }, 12000);
  if (!senderReady) {
    auto *label = widget.findChild<QLabel *>(QStringLiteral("audioStatusLabel"));
    std::cerr << "sender_status=" << (label ? label->text().toStdString() : "missing")
              << " receiver_connected=" << receiver.connected << " done=" << receiver.done
              << " packets=" << receiver.packets << " muted=" << muteIs(true) << '\n';
  }
  check(senderReady, "Actual sender did not connect and default-mute");
  check(!local->isChecked() && stop->isVisible(), "Sender checkbox/stop visibility failed");
  if (!remoteStop) {
    check(widget.grab().save(QApplication::applicationDirPath() + QStringLiteral("/widget_sender.png")),
          "Cannot save sender widget evidence");
    if (!originallyMuted) {
      const auto initiallyReceived = receiver.packets.load();
      local->setChecked(true);
      check(waitFor([&] { return muteIs(false); }, 1500), "Checkbox did not safely restore local output");
      check(waitFor([&] { return receiver.packets > initiallyReceived + 50; }, 1500), "SRTP stalled during local playback");
      local->setChecked(false);
      check(waitFor([&] { return muteIs(true); }, 1500), "Checkbox did not reacquire local mute");
    }
    failSenderWidgetReceive(); // Real worker exception, isolated to the fixture's sender recvfrom.
    check(waitFor([&] { return local->isHidden() && stop->isVisible(); }, 1500),
          "Retry did not leave a usable stop control");
    receiver.join();
    QElapsedTimer retryGap; retryGap.start();
    while (retryGap.elapsed() < 2300) {
      QApplication::processEvents();
      check(muteIs(true), "Sender became audible while waiting for retry");
      check(watchdogChildren() == 1, "Retry destroyed the native mute owner");
      QThread::msleep(20);
    }
    session = QUuid::createUuid().toString(QUuid::Id128);
    preview::SessionOptions resumed; resumed.receive = true; resumed.test = true; resumed.controlOwnsLifetime = true;
    resumed.onReady = [&](const preview::Invitation &offer) {
      const auto payload = offerJson(offer);
      QMetaObject::invokeMethod(&callbacks, [&, payload] {
        remote.send(peer, {{"type", "offer"}, {"session", session}, {"offer", payload}});
      }, Qt::QueuedConnection);
    };
    receiver.start(std::move(resumed));
    check(waitFor([&] { return receiver.connected && local->isVisible(); }, 12000) && muteIs(true),
          "Sender did not reconnect without releasing mute");
  }
  // Second session remains muted throughout: its decoded peak specifically
  // proves endpoint mute does not silence WASAPI loopback capture.
  check(waitFor([&] { return receiver.done.load(); }, 7000), "Remote decode did not finish");
  check(receiver.succeeded && receiver.packets >= 600 && receiver.peak > 0,
        "Actual widget capture/SRTP did not decode non-silent audio");
  const auto verifiedPackets = receiver.packets.load();
  const auto verifiedPeak = receiver.peak.load();
  if (remoteStop)
    remote.send(peer, {{"type", "state"}, {"listen", false}, {"available", true}});
  else {
    // Also exercise a fresh offer arriving before its old sender sees STOP.
    // A ready sender must supersede the stale session rather than ignore it.
    session = QUuid::createUuid().toString(QUuid::Id128);
    preview::SessionOptions replacement; replacement.receive = true; replacement.test = true;
    replacement.controlOwnsLifetime = true;
    replacement.onReady = [&](const preview::Invitation &offer) {
      const auto payload = offerJson(offer);
      QMetaObject::invokeMethod(&callbacks, [&, payload] {
        remote.send(peer, {{"type", "offer"}, {"session", session}, {"offer", payload}});
      }, Qt::QueuedConnection);
    };
    receiver.start(std::move(replacement));
    check(waitFor([&] { return receiver.connected.load(); }, 12000) && muteIs(true),
          "Fresh trusted offer was ignored while an old sender was active");
    remote.send(peer, {{"type", "stop"}, {"session", session}, {"retry", true}});
    check(waitFor([&] { return local->isHidden() && stop->isVisible(); }, 1500) && muteIs(true),
          "Remote retry released the sender mute");
    stop->click();
  }
  check(waitFor([&] {
    const bool expectedButton = remoteStop ? stop->isHidden()
                                           : stop->isVisible() && stop->text() == QStringLiteral("恢复共享本机声音");
    return muteIs(originallyMuted) && expectedButton && local->isHidden() && watchdogChildren() == 0;
  }, 2000), "Stop did not restore original mute or show the correct resume control");
  check(waitFor([&] { return stopMessages > 0; }, 1000), "Session stop was not advertised");
  check(disconnected == 0 && control->peerIds().size() == 1 && remote.peerIds().size() == 1,
        "Media stop disconnected paired TLS");
  check(!dialogs.seen && !QApplication::activeModalWidget(), "Unexpected audio authorization dialog");
  if (!remoteStop) {
    stop->click();
    check(stop->isHidden() && stop->text() == QStringLiteral("停止共享本机声音"),
          "Resume did not restore the sender's available state");
  }
  check(contents(clientsDb) == clientsBefore && contents(serversDb) == serversBefore &&
        QDir(temporary.path()).entryList(QDir::Files) == filesBefore, "Pairing/files changed");
  std::cout << "actual_sender=1 default_mute=1 checkbox_restore_remute=" << (!remoteStop && !originallyMuted)
            << " entire_session_muted=" << remoteStop << " decoded=" << verifiedPackets
            << " decoded_peak=" << verifiedPeak << " stop_source=" << (remoteStop ? "remote_state" : "button")
            << " worker_failure_retry=" << !remoteStop << " remote_retry=" << !remoteStop
            << " fresh_offer_replaces_sender=" << !remoteStop
            << " original_mute_restored=1 tls_survives=1 dialogs=0 invitation_files=0\n";
}
} // namespace
int main(int argc, char **argv)
{
  std::cout << "Explicit sender device test: quiet system tone, real loopback, temporary endpoint mute.\n";
  QApplication::setApplicationName(kAppName);
  QApplication::setOrganizationName(QStringLiteral("DeskflowIntegrationTest"));
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  float originalVolume = 0; bool originalMute = false;
  if (WasapiBackend::readDefaultMasterVolume(originalVolume, originalMute) < 0) {
    std::cout << "SKIP: requires an available endpoint.\n";
    return 77;
  }
  QTemporaryDir temporary;
  if (!temporary.isValid()) return 1;
  qputenv("XDG_STATE_HOME", temporary.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
  const auto folder = QApplication::applicationDirPath() + QStringLiteral("/settings");
  if (!QDir().mkpath(folder)) return 2;
  QLockFile lock(folder + QStringLiteral("/audio-smoke.lock"));
  if (!lock.tryLock(10000)) return 3;
  QFile config(folder + QStringLiteral("/") + kAppName + QStringLiteral(".conf"));
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 4;
  const auto configuration = config.fileName();
  config.write("[gui]\naudioListen=false\naudioLocalPlayback=false\nstartCoreWithGui=false\nenableUpdateCheck=false\n"
               "[security]\ntlsEnabled=false\n[core]\nprocessMode=1\ncomputerName=sender-widget-smoke\n");
  config.close();
  struct Fixture
  {
    QString path;
    ~Fixture() { Settings::save(); QFile::remove(path); }
  } fixture{configuration};
  try {
    check(Settings::settingsFile() == configuration, "Settings escaped isolated fixture");
    const auto serverPem = temporary.filePath(QStringLiteral("server.pem"));
    const auto clientPem = temporary.filePath(QStringLiteral("client.pem"));
    deskflow::generatePemSelfSignedCert(serverPem, 2048);
    deskflow::generatePemSelfSignedCert(clientPem, 2048);
    check(trust(temporary.filePath(QStringLiteral("clients")), clientPem) &&
          trust(temporary.filePath(QStringLiteral("servers")), serverPem), "Cannot trust test identities");
    Tone tone;
    deskflow::audio::windows::Callbacks callbacks;
    callbacks.context = &tone; callbacks.render = &Tone::render; callbacks.status = &Tone::status;
    WasapiBackend source(deskflow::audio::windows::Direction::Render, callbacks);
    check(source.start(), "Cannot start isolated quiet system source");
    check(waitFor([&] { return tone.running && tone.callbacks > 5; }, 5000), "Quiet source did not open");
    exercise(app, temporary, false, originalMute);
    exercise(app, temporary, true, originalMute);
    source.stop();
    float finalVolume = 0; bool finalMute = true;
    check(WasapiBackend::readDefaultMasterVolume(finalVolume, finalMute) >= 0 && finalMute == originalMute &&
          std::abs(finalVolume - originalVolume) < .000001f, "Original endpoint mute/volume not preserved");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Sender widget test failed: " << error.what() << '\n';
    return 5;
  }
}
