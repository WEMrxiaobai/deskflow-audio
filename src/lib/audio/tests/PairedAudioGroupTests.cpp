// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Three real widgets + original-style mTLS pairing + direct Opus/SRTP sources.
// Shared synthetic playback performs the real mixer without opening any device.
#include "common/Constants.h"
#include "common/Settings.h"
#include "gui/audio/PairedAudioControl.h"
#include "gui/audio/PairedAudioGroup.h"
#include "gui/widgets/AudioSharingWidget.h"
#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QSlider>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

void failGroupReceiver(std::size_t slot);

namespace {
void check(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()> &predicate, int timeout = 15000)
{
  QElapsedTimer timer;
  timer.start();
  do {
    QApplication::processEvents();
    if (predicate())
      return true;
    QThread::msleep(2);
  } while (timer.elapsed() < timeout);
  return predicate();
}
void pump(int milliseconds)
{
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < milliseconds) {
    QApplication::processEvents();
    QThread::msleep(2);
  }
}
Fingerprint fingerprint(const QString &pem)
{
  QFile file(pem);
  check(file.open(QIODevice::ReadOnly), "Cannot read isolated certificate");
  return {QCryptographicHash::Sha256, QSslCertificate(file.readAll(), QSsl::Pem).digest(QCryptographicHash::Sha256)};
}
QByteArray contents(const QString &path)
{
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "Cannot read isolated trust database");
  return file.readAll();
}
struct Node
{
  AudioSharingWidget widget;
  PairedAudioControl *control = widget.findChild<PairedAudioControl *>();
  PairedAudioGroup *group = widget.findChild<PairedAudioGroup *>();
  QCheckBox *listen = widget.findChild<QCheckBox *>(QStringLiteral("audioListenSwitch"));
  explicit Node(bool render)
  {
    check(control && group && listen, "Missing production group integration");
    group->setTestMode(render);
  }
  ~Node()
  {
    control->configure(false, false, {});
    group->shutdown();
  }
};
bool receiving(Node &node, std::size_t count)
{
  const auto sources = node.group->sources();
  if (sources.size() != count)
    return false;
  for (const auto &source : sources)
    if (source.status != QStringLiteral("正在接收") || source.decodedPackets < 100)
      return false;
  return true;
}
std::shared_ptr<preview::SourcePlaybackControl> sourceControl(Node &node, const QString &id)
{
  for (const auto &source : node.group->sources())
    if (source.id == id)
      return source.control;
  return {};
}
void exercise(const QTemporaryDir &temporary, bool render)
{
  const auto hubPem = temporary.filePath("hub.pem"), alphaPem = temporary.filePath("alpha.pem"),
             betaPem = temporary.filePath("beta.pem");
  for (const auto &pem : {hubPem, alphaPem, betaPem})
    deskflow::generatePemSelfSignedCert(pem, 2048);
  const auto hubDb = temporary.filePath("trusted-clients"), alphaDb = temporary.filePath("alpha-servers"),
             betaDb = temporary.filePath("beta-servers");
  FingerprintDatabase clients, servers;
  clients.addTrusted(fingerprint(alphaPem));
  clients.addTrusted(fingerprint(betaPem));
  servers.addTrusted(fingerprint(hubPem));
  check(clients.write(hubDb) && servers.write(alphaDb) && servers.write(betaDb), "Cannot establish original trust");
  const auto hubBefore = contents(hubDb), alphaBefore = contents(alphaDb), betaBefore = contents(betaDb);
  const auto filesBefore = QDir(temporary.path()).entryList(QDir::Files);
  const auto alphaId = QString::fromLatin1(fingerprint(alphaPem).data.toHex());
  const auto betaId = QString::fromLatin1(fingerprint(betaPem).data.toHex());
  QTcpServer reservation;
  check(reservation.listen(QHostAddress::LocalHost, 0), "Cannot reserve test control port");
  const auto port = reservation.serverPort();
  reservation.close();
  Node hub(render), alpha(render), beta(render);
  auto profile = [&](const QString &pem, const QString &db, const QString &name, bool server) {
    PairedAudioControl::Configuration result;
    result.useSettings = false;
    result.port = port;
    result.certificatePath = pem;
    result.localName = name;
    if (server)
      result.trustedClientsPath = db;
    else
      result.trustedServersPath = db;
    return result;
  };
  hub.control->setConfiguration(profile(hubPem, hubDb, "hub", true));
  alpha.control->setConfiguration(profile(alphaPem, alphaDb, "alpha", false));
  beta.control->setConfiguration(profile(betaPem, betaDb, "beta", false));
  QObject callbacks;
  int betaOffers = 0;
  QString betaRoute;
  bool hubSending = false, alphaSending = false, betaSending = false;
  QObject::connect(hub.group, &PairedAudioGroup::senderChanged, &callbacks, [&](bool value, bool) {
    hubSending = value;
  });
  QObject::connect(alpha.group, &PairedAudioGroup::senderChanged, &callbacks, [&](bool value, bool) {
    alphaSending = value;
  });
  QObject::connect(beta.group, &PairedAudioGroup::senderChanged, &callbacks, [&](bool value, bool) {
    betaSending = value;
  });
  QObject::connect(
      beta.control, &PairedAudioControl::messageReceived, &callbacks,
      [&](const QString &, const QJsonObject &message) {
        if (message.value("type") == "group-offer")
          ++betaOffers;
        if (message.value("type") == "group-plan" && message.value("role") == "send")
          betaRoute = message.value("send").toObject().value("route").toString();
      }
  );
  hub.listen->setChecked(true);
  hub.control->configure(true, true, {}, {"alpha", "beta"});
  alpha.control->configure(false, true, "127.0.0.1");
  beta.control->configure(false, true, "127.0.0.1");
  check(
      waitFor([&] { return receiving(hub, 2) && alphaSending && betaSending && !hubSending; }),
      "Server listener did not establish two independent sources"
  );
  auto output = hub.group->playbackForTest();
  check(
      output && waitFor([&] { return output->twoSourceCallbacks() > 100 && output->peak() > 0; }),
      "Both sources did not enter the single mixer"
  );
  check(std::isfinite(output->peak()) && output->peak() <= .98f, "Mixed output escaped the limiter");
  auto stableBeta = sourceControl(hub, betaId);
  const auto initialSources = hub.group->sources();
  int alphaRow = initialSources[0].id == alphaId ? 0 : 1;
  auto *gain = hub.widget.findChild<QSlider *>(QStringLiteral("audioSourceVolume%1").arg(alphaRow));
  auto *mute = hub.widget.findChild<QCheckBox *>(QStringLiteral("audioSourceMute%1").arg(alphaRow));
  check(gain && mute, "Missing per-source GUI controls");
  gain->setValue(19);
  mute->setChecked(true);
  auto alphaControls = sourceControl(hub, alphaId);
  check(
      alphaControls && std::abs(alphaControls->volume.load() - .19f) < .0001f && alphaControls->muted,
      "Per-source controls were not applied"
  );
  const auto offersBeforeFailure = betaOffers;
  failGroupReceiver(initialSources[alphaRow].playbackSlot);
  check(
      waitFor([&] {
        for (const auto &source : hub.group->sources())
          if (source.id == alphaId)
            return source.status == QStringLiteral("正在重连");
        return false;
      }),
      "Injected source failure did not enter retry"
  );
  const auto callbacksBeforeRetry = output->renderedCallbacks();
  check(
      waitFor([&] { return receiving(hub, 2); }) && betaOffers == offersBeforeFailure &&
          sourceControl(hub, alphaId) == alphaControls && sourceControl(hub, betaId) == stableBeta &&
          output == hub.group->playbackForTest() && output->renderedCallbacks() > callbacksBeforeRetry + 100 &&
          hub.control->peerIds().size() == 2,
      "Media-only retry restarted the healthy source/output or lost its TLS pairing"
  );
  alpha.control->configure(false, false, {});
  check(
      waitFor([&] { return receiving(hub, 1) && hub.group->sources()[0].id == betaId; }),
      "Disconnecting one source did not leave the other running"
  );
  const auto previousCallbacks = output->renderedCallbacks();
  pump(1200);
  check(
      output->renderedCallbacks() > previousCallbacks + 100 && sourceControl(hub, betaId) == stableBeta &&
          hub.group->playbackForTest() == output,
      "Other source or playback was recreated on disconnect"
  );
  alpha.control->configure(false, true, "127.0.0.1");
  check(waitFor([&] { return receiving(hub, 2); }), "Source did not reconnect automatically");
  alphaControls = sourceControl(hub, alphaId);
  check(
      alphaControls && alphaControls->muted && std::abs(alphaControls->volume.load() - .19f) < .0001f &&
          sourceControl(hub, betaId) == stableBeta && hub.group->playbackForTest() == output,
      "Reconnect lost saved gain/mute or restarted the healthy source"
  );
  output.reset();
  alpha.listen->setChecked(true); // Server still owns the listener while its switch is on.
  pump(200);
  check(receiving(hub, 2) && alpha.group->sources().empty(), "Concurrent listener requests formed a loop");
  hub.listen->setChecked(false);
  check(
      waitFor([&] {
        return receiving(alpha, 2) && hubSending && betaSending && !alphaSending && hub.group->sources().empty() &&
               beta.group->sources().empty();
      }),
      "Client listener did not receive server plus the other client via brokered direct routes"
  );
  auto clientOutput = alpha.group->playbackForTest();
  check(
      clientOutput && waitFor([&] { return clientOutput->twoSourceCallbacks() > 100; }),
      "Client listener did not mix both direct sources"
  );
  const auto beforeSpoof = betaOffers;
  check(!betaRoute.isEmpty(), "Missing broker route evidence");
  beta.control->send(
      beta.control->peerIds().first(), {{"type", "group-offer"}, {"route", betaRoute}, {"offer", QJsonObject{}}}
  );
  beta.control->send(beta.control->peerIds().first(), {{"type", "group-plan"}, {"role", "receive"}});
  pump(300);
  check(
      betaOffers == beforeSpoof && receiving(alpha, 2) && hub.group->sources().empty(),
      "A source could impersonate the listener or coordinator"
  );
  auto stableHub = sourceControl(alpha, QStringLiteral("server"));
  beta.control->configure(false, false, {});
  check(
      waitFor([&] { return receiving(alpha, 1); }) && sourceControl(alpha, QStringLiteral("server")) == stableHub,
      "Client listener lost its server source when the other client left"
  );
  clientOutput.reset();
  beta.control->configure(false, true, "127.0.0.1");
  check(waitFor([&] { return receiving(alpha, 2); }), "Delegated source failed to reconnect");
  beta.listen->setChecked(true);
  pump(150);
  check(receiving(alpha, 2) && beta.group->sources().empty(), "Two client listeners were active concurrently");
  hub.listen->setChecked(true);
  check(
      waitFor([&] {
        return receiving(hub, 2) && alpha.group->sources().empty() && beta.group->sources().empty() && !hubSending;
      }),
      "Server priority failed to serialize three simultaneous listener requests"
  );
  hub.listen->setChecked(false);
  alpha.listen->setChecked(false);
  beta.listen->setChecked(false);
  check(
      waitFor([&] {
        return hub.group->sources().empty() && alpha.group->sources().empty() && beta.group->sources().empty() &&
               !hubSending && !alphaSending && !betaSending;
      }),
      "Stopping all listeners left an active source or mixer"
  );
  beta.control->send(beta.control->peerIds().first(), {{"type", "state"}, {"listen", false}, {"available", false}});
  check(
      // Beta is advertising the old protocol; only the hub and alpha remain
      // group-capable peers. A real old beta has no PairedAudioGroup at all.
      waitFor([&] { return !hub.group->enabled() && !alpha.group->enabled(); }),
      "A legacy peer did not disable group mode on the remaining capable endpoints"
  );
  beta.control->send(
      beta.control->peerIds().first(), {{"type", "state"}, {"listen", false}, {"available", true}, {"audioVersion", 2}}
  );
  check(
      waitFor([&] { return hub.group->enabled() && alpha.group->enabled() && beta.group->enabled(); }),
      "Group mode did not recover after the legacy peer upgraded"
  );
  check(
      contents(hubDb) == hubBefore && contents(alphaDb) == alphaBefore && contents(betaDb) == betaBefore &&
          QDir(temporary.path()).entryList(QDir::Files) == filesBefore,
      "Group routing changed trust or created invitation files"
  );
  std::cout << "three_widgets=pass server_listener_two_sources=pass client_listener_two_sources=pass "
               "single_mixer=pass media_only_retry=pass independent_disconnect_reconnect=pass "
               "source_gain_mute_persistence=pass "
               "listener_election=pass broker_spoof_rejected=pass legacy_downgrade_recovery=pass trust_unchanged=pass "
               "real_render="
            << render << '\n';
}
} // namespace
int main(int argc, char **argv)
{
  QApplication::setApplicationName(kAppName);
  QApplication::setOrganizationName(QStringLiteral("DeskflowIntegrationTest"));
  QApplication app(argc, argv);
  if (app.arguments().contains("--verify-saved")) {
    const auto expected = QApplication::applicationDirPath() + "/settings/" + kAppName + ".conf";
    if (!QFile::exists(expected) || Settings::settingsFile() != expected)
      return 10;
    const auto saved = Settings::value(Settings::Gui::AudioSourceControls).toMap();
    for (const auto &item : saved) {
      const auto entry = item.toMap();
      if (entry.value("muted").toBool() && std::abs(entry.value("volume").toFloat() - .19f) < .0001f)
        return 0;
    }
    return 11;
  }
  QTemporaryDir temporary;
  if (!temporary.isValid())
    return 1;
  qputenv("XDG_STATE_HOME", temporary.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
  const auto folder = QApplication::applicationDirPath() + "/settings";
  if (!QDir().mkpath(folder))
    return 2;
  QLockFile lock(folder + "/audio-smoke.lock");
  if (!lock.tryLock(10000))
    return 3;
  QFile config(folder + "/" + kAppName + ".conf");
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return 4;
  const auto configuration = config.fileName();
  config.write(
      "[gui]\naudioListen=false\naudioLocalPlayback=false\nstartCoreWithGui=false\nenableUpdateCheck=false\n"
      "[security]\ntlsEnabled=false\n[core]\ncomputerName=audio-group-hub\nprocessMode=1\n"
  );
  config.close();
  int result = 0;
  try {
    check(Settings::settingsFile() == configuration, "Settings escaped the isolated fixture");
    exercise(temporary, app.arguments().contains("--render"));
    check(
        QProcess::execute(QApplication::applicationFilePath(), {"--verify-saved"}) == 0,
        "Saved per-source gain/mute did not survive a fresh settings process"
    );
  } catch (const std::exception &error) {
    std::cerr << "Three-machine group test failed: " << error.what() << '\n';
    result = 5;
  }
  Settings::save();
  QFile::remove(configuration);
  return result;
}
