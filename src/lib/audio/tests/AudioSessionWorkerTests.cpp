/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// 隔离合成测试：链接下方 runSession 替身，不打开声卡、不启动 core、不改变用户信任。
#include "Session.h"
#include "common/Constants.h"
#include "common/Settings.h"
#include "gui/audio/AudioSessionWorker.h"
#include "gui/audio/PairedAudioControl.h"
#include "gui/widgets/AudioSharingWidget.h"
#include "net/FingerprintDatabase.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QPushButton>
#include <QSslCertificate>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
struct Fixture
{
  std::atomic<int> started{0}, cleaned{0}, policyReads{0};
  std::atomic<bool> cleanupAllowed{false}, stopping{false}, fail{false}, lateCallbacks{false};
} fixture;

void check(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}

// 只在测试中泵事件；所有耗时清理由原子屏障控制，避免依靠线程恰好慢于某个时间点。
bool waitFor(const std::function<bool()> &predicate, int timeout = 5000)
{
  QElapsedTimer clock;
  clock.start();
  do {
    QApplication::processEvents();
    if (predicate())
      return true;
    QThread::msleep(2);
  } while (clock.elapsed() < timeout);
  return predicate();
}

void resetFixture()
{
  fixture.cleanupAllowed = false;
  fixture.stopping = false;
  fixture.fail = false;
  fixture.lateCallbacks = false;
}
} // namespace

namespace preview {
// 真实任务包装器照常运行，替换的仅是阻塞媒体函数；模拟失败、清理卡住及销毁后的晚到回调。
void runSession(const SessionOptions &options, std::atomic<bool> &stop, const SessionObserver &observer)
{
  ++fixture.started;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!stop && !fixture.fail && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  fixture.stopping = true;
  while (!fixture.cleanupAllowed && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (options.localPlayback) {
    (void)options.localPlayback->load();
    ++fixture.policyReads;
  }
  if (fixture.lateCallbacks) {
    Invitation offer;
    offer.mediaKey.fill(0x35);
    // 替身也遵守角色边界；发送端没有邀请发布回调，真实参数和媒体由独立集成测试覆盖。
    if (options.onReady)
      options.onReady(offer);
    erase(offer);
    if (options.onRecovery)
      options.onRecovery({false, 12});
    observer({SessionState::LocalPlaybackOverridden, "synthetic override"});
    // 邮箱只保留最新可见状态，不能把这一万次通知变成一万个 Qt 排队回调。
    for (int i = 0; i < 10000; ++i)
      observer({SessionState::Connected, "synthetic status", static_cast<std::uint64_t>(i)});
    for (int i = 0; i < 100; ++i)
      observer({SessionState::Diagnostic, "synthetic diagnostic", static_cast<std::uint64_t>(i)});
  }
  ++fixture.cleaned;
  if (fixture.fail)
    throw std::runtime_error("synthetic audio failure");
}
} // namespace preview

// 仅此测试访问控件的音频入口；不注入真实键鼠连接，也不放宽产品的身份验证门控。
class AudioSharingWidgetTestAccess
{
public:
  static void start(AudioSharingWidget &widget)
  {
    widget.startSession(QStringLiteral("test-peer"), false, QString(32, 'a'));
  }
  static void stop(AudioSharingWidget &widget)
  {
    widget.stopSession(false, "synthetic-stop");
  }
  static bool idle(const AudioSharingWidget &widget)
  {
    return !widget.m_running && !widget.m_worker;
  }
  static bool running(const AudioSharingWidget &widget)
  {
    return widget.m_running;
  }
  static bool pending(const AudioSharingWidget &widget)
  {
    return static_cast<bool>(widget.m_pendingOffer);
  }
};

namespace {
// 测试证书仅写临时目录，使用独立控制配置；不读取或改变产品身份和信任数据库。
void makeIdentity(const QString &openssl, const QString &pem, const QString &trustPath)
{
  QProcess process;
  process.start(
      openssl, {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=audio-worker-test",
                "-keyout", pem, "-out", pem + ".crt"}
  );
  check(process.waitForFinished(15000) && process.exitCode() == 0, "cannot generate isolated certificate");
  QFile certificate(pem + ".crt"), combined(pem);
  check(certificate.open(QIODevice::ReadOnly) && combined.open(QIODevice::Append), "cannot read test identity");
  const auto bytes = certificate.readAll();
  check(combined.write(bytes) == bytes.size(), "cannot combine test identity");
  FingerprintDatabase database;
  database.addTrusted({QCryptographicHash::Sha256, QSslCertificate(bytes).digest(QCryptographicHash::Sha256)});
  check(database.write(trustPath), "cannot write isolated trust");
}

// 真实 mTLS 传递 stop/offer；只有媒体函数是合成替身，检查清理期间的邀请等待、取消及音频故障隔离。
void exerciseControl(const QString &openssl)
{
  resetFixture();
  QTemporaryDir temporary;
  check(temporary.isValid(), "cannot create control fixture");
  const auto senderPem = temporary.filePath("sender.pem"), receiverPem = temporary.filePath("receiver.pem");
  const auto senders = temporary.filePath("senders"), receivers = temporary.filePath("receivers");
  makeIdentity(openssl, senderPem, senders);
  makeIdentity(openssl, receiverPem, receivers);
  QTcpServer probe;
  check(probe.listen(QHostAddress::LocalHost, 0), "cannot reserve TLS port");
  const auto port = probe.serverPort();
  probe.close();
  AudioSharingWidget widget;
  auto *control = widget.findChild<PairedAudioControl *>();
  PairedAudioControl remote;
  QObject callbacks;
  int disconnects = 0;
  int deliveredOffers = 0;
  QObject::connect(control, &PairedAudioControl::peerDisconnected, &callbacks, [&](const QString &) { ++disconnects; });
  // 该观察器在控件自身的消息处理器之后运行；计数推进即表示邀请已经完成真实 TLS 解帧和 GUI 校验。
  // 用它作为拒绝分支的事件屏障，避免仅凭短暂等待就误判“没有启动任务”。
  QObject::connect(
      control, &PairedAudioControl::messageReceived, &callbacks,
      [&](const QString &, const QJsonObject &message) {
        if (message.value("type") == QStringLiteral("offer"))
          ++deliveredOffers;
      }
  );
  PairedAudioControl::Configuration sender, receiver;
  sender.useSettings = receiver.useSettings = false;
  sender.certificatePath = senderPem;
  sender.trustedClientsPath = receivers;
  sender.localName = "synthetic-sender";
  receiver.certificatePath = receiverPem;
  receiver.trustedServersPath = senders;
  receiver.localName = "synthetic-receiver";
  sender.port = receiver.port = port;
  control->setConfiguration(sender);
  remote.setConfiguration(receiver);
  control->configure(true, true, {}, {receiver.localName});
  remote.configure(false, true, "127.0.0.1");
  check(waitFor([&] { return control->peerIds().size() == 1 && remote.peerIds().size() == 1; }), "TLS did not pair");
  const auto peer = remote.peerIds().first();
  remote.send(peer, {{"type", "state"}, {"listen", true}, {"available", true}, {"recovery", true}});
  auto sendOffer = [&](char id, std::uint64_t expires = 0) {
    auto invitation = preview::createInvitation(12345, {"127.0.0.1"});
    // 仅改变来自可信对端的到期时间，模拟不同机器的墙上时钟；不修改系统时间或媒体单调时钟。
    if (expires)
      invitation.expiresUnixSeconds = expires;
    const QJsonObject offer{
        {"media",
         QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(invitation.mediaKey.data()), 30).toBase64())},
        {"control",
         QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(invitation.controlKey.data()), 32).toBase64())},
        {"ssrc", QString::number(invitation.ssrc)},
        {"expires", QString::number(invitation.expiresUnixSeconds)},
        {"port", 12345}
    };
    remote.send(peer, {{"type", "offer"}, {"session", QString(32, id)}, {"offer", offer}});
    preview::erase(invitation);
  };
  const auto initial = fixture.started.load();
  // 容忍正常时钟偏差不能取消凭据边界：过期和超出媒体层上限的邀请仍须拒绝，且不启动声卡任务。
  sendOffer('x', preview::unixSeconds() - 1);
  check(waitFor([&] { return deliveredOffers == 1; }), "expired TLS offer was not delivered");
  check(AudioSharingWidgetTestAccess::idle(widget), "expired TLS offer started audio");
  sendOffer('y', preview::unixSeconds() + 670);
  check(waitFor([&] { return deliveredOffers == 2; }), "future TLS offer was not delivered");
  check(AudioSharingWidgetTestAccess::idle(widget), "excessively future TLS offer started audio");
  check(fixture.started == initial, "invalid TLS offers started a background task");
  // 邀请在对端仍有十分钟有效期；本机时钟慢十秒时会观察到 610 秒，必须接受而非静默丢弃。
  // 此处走控件的真实接收校验和任务包装器，避免只测试绕过 GUI 的 Session 媒体接口。
  sendOffer('a', preview::unixSeconds() + 610);
  check(waitFor([&] { return fixture.started == initial + 1; }), "clock-skewed TLS offer did not start audio");
  remote.send(peer, {{"type", "stop"}, {"session", QString(32, 'a')}});
  check(waitFor([] { return fixture.stopping.load(); }), "TLS stop did not reach worker");
  sendOffer('b');
  check(waitFor([&] { return AudioSharingWidgetTestAccess::pending(widget); }), "offer was lost during cleanup");
  remote.send(peer, {{"type", "stop"}, {"session", QString(32, 'b')}});
  check(waitFor([&] { return !AudioSharingWidgetTestAccess::pending(widget); }), "pending offer ignored stop");
  sendOffer('c');
  check(waitFor([&] { return AudioSharingWidgetTestAccess::pending(widget); }), "replacement offer was lost");
  check(fixture.started == initial + 1, "TLS offer bypassed cleanup gate");
  fixture.cleanupAllowed = true;
  check(waitFor([&] { return fixture.started == initial + 2; }), "pending offer did not start after cooldown");
  auto *stopButton = widget.findChild<QPushButton *>(QStringLiteral("audioStopButton"));
  check(stopButton != nullptr, "missing stop button");
  stopButton->click();
  check(waitFor([&] { return AudioSharingWidgetTestAccess::idle(widget); }), "button stop did not clean up");
  check(
      disconnects == 0 && control->peerIds().size() == 1 && remote.peerIds().size() == 1,
      "audio stops/retries disconnected TLS control"
  );
}

void exerciseWidget()
{
  resetFixture();
  auto widget = std::make_unique<AudioSharingWidget>();
  const auto initial = fixture.started.load();
  AudioSharingWidgetTestAccess::start(*widget);
  check(waitFor([&] { return fixture.started > initial; }), "worker did not start");
  QElapsedTimer latency;
  latency.start();
  AudioSharingWidgetTestAccess::stop(*widget);
  check(latency.elapsed() < 100, "audio stop blocked the GUI");
  check(waitFor([] { return fixture.stopping.load(); }), "stop was not observed");
  int ticks = 0;
  QTimer inputLoop;
  inputLoop.setInterval(5);
  QObject::connect(&inputLoop, &QTimer::timeout, [&] { ++ticks; });
  inputLoop.start();
  check(waitFor([&] { return ticks >= 10; }), "GUI event loop stalled during cleanup");
  // 即使等待已越过两秒，清理屏障仍不允许叠加任务；重复入口也不能产生后台线程风暴。
  check(waitFor([&] { return latency.elapsed() >= 2100; }), "test clock did not advance");
  for (int i = 0; i < 100; ++i)
    AudioSharingWidgetTestAccess::start(*widget);
  check(fixture.started == initial + 1, "new session overlapped pending cleanup");
  fixture.lateCallbacks = true;
  latency.restart();
  widget.reset();
  check(latency.elapsed() < 100, "widget destructor blocked the GUI");
  auto replacement = std::make_unique<AudioSharingWidget>();
  AudioSharingWidgetTestAccess::start(*replacement);
  check(!AudioSharingWidgetTestAccess::running(*replacement), "new widget bypassed process cleanup lease");
  fixture.cleanupAllowed = true;
  check(waitFor([&] { return fixture.cleaned == initial + 1; }), "background cleanup did not finish");
  check(fixture.policyReads > 0, "background policy was not alive after widget destruction");
  // 等待新控件的真实冷却时间；先前控件的晚到通知不能更改它的会话。
  check(
      waitFor([&] {
        AudioSharingWidgetTestAccess::start(*replacement);
        return AudioSharingWidgetTestAccess::running(*replacement);
      }),
      "audio did not restart after cleanup"
  );
  AudioSharingWidgetTestAccess::stop(*replacement);
  check(waitFor([&] { return AudioSharingWidgetTestAccess::idle(*replacement); }), "replacement did not clean up");
}

void exerciseRetryAndMailbox()
{
  resetFixture();
  fixture.cleanupAllowed = true;
  fixture.fail = true;
  auto widget = std::make_unique<AudioSharingWidget>();
  const auto initial = fixture.started.load();
  AudioSharingWidgetTestAccess::start(*widget);
  check(waitFor([&] { return AudioSharingWidgetTestAccess::idle(*widget); }), "failure did not finish");
  for (int i = 0; i < 100; ++i)
    AudioSharingWidgetTestAccess::start(*widget);
  check(fixture.started == initial + 1, "immediate retry bypassed two-second gate");
  check(
      waitFor([&] {
        AudioSharingWidgetTestAccess::start(*widget);
        return fixture.started == initial + 2;
      }),
      "retry gate never reopened"
  );
  check(waitFor([&] { return AudioSharingWidgetTestAccess::idle(*widget); }), "retry did not finish");
  widget.reset();

  resetFixture();
  fixture.fail = true;
  fixture.lateCallbacks = true;
  fixture.cleanupAllowed = true;
  preview::SessionOptions options;
  // 邀请邮箱属于接收方向，测试不能依赖发送端错误地安装 onReady 才得到邀请。
  options.receive = true;
  options.recovery = std::make_shared<preview::SessionRecovery>();
  AudioSessionWorker worker(std::move(options), true);
  AudioSessionWorker::Update update;
  int offers = 0, recoveries = 0;
  std::uint64_t lastPackets = 0;
  std::uint64_t lastDiagnostic = 0;
  bool overridden = false;
  check(
      waitFor([&] {
        update = worker.takeUpdate();
        offers += update.offer.has_value();
        recoveries += update.recovery.has_value();
        if (update.status)
          lastPackets = update.status->packets;
        check(update.logs.size() <= 8, "diagnostic mailbox exceeded capacity");
        if (!update.logs.empty())
          lastDiagnostic = update.logs.back().packets;
        overridden |= update.playbackOverridden;
        return update.finished;
      }),
      "mailbox worker did not finish"
  );
  // finished 前轮询可能已经消费邀请，因此另取一次不应重新交付任何已消费的单槽。
  check(overridden && !update.failure.empty(), "sticky state/failure was lost");
  check(offers == 1 && recoveries == 1 && lastPackets == 9999, "bounded mailbox lost final/critical state");
  check(lastDiagnostic == 99, "bounded logs lost final diagnostic");
  const auto empty = worker.takeUpdate();
  check(!empty.offer && !empty.recovery && !empty.status, "mailbox redelivered consumed messages");
}
} // namespace

int main(int argc, char **argv)
{
  QApplication::setApplicationName(kAppName);
  QApplication::setOrganizationName(QStringLiteral("DeskflowWorkerTest"));
  QApplication app(argc, argv);
  // 在第一次 Settings 访问前创建独立便携配置；拒绝覆盖任何已有文件。
  const auto folder = QApplication::applicationDirPath() + QStringLiteral("/settings");
  if (!QDir().mkpath(folder))
    return 2;
  QLockFile lock(folder + QStringLiteral("/audio-worker-test.lock"));
  if (!lock.tryLock(1000))
    return 3;
  QFile config(folder + '/' + kAppName + QStringLiteral(".conf"));
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return 4;
  config.write("[gui]\naudioListen=false\naudioLocalPlayback=false\nstartCoreWithGui=false\n");
  config.close();
  struct ConfigCleanup
  {
    QString path;
    ~ConfigCleanup()
    {
      Settings::save();
      QFile::remove(path);
    }
  } cleanup{config.fileName()};
  try {
    check(Settings::settingsFile() == config.fileName(), "settings escaped isolated fixture");
    exerciseWidget();
    exerciseRetryAndMailbox();
    check(argc == 2, "pass the openssl executable as the first argument");
    exerciseControl(QString::fromLocal8Bit(argv[1]));
    std::cout << "nonblocking_stop=1 nonblocking_destructor=1 cleanup_exclusive=1 gui_progress=1 "
                 "late_callbacks_safe=1 retry_gate=1 bounded_mailbox=1 pending_offer=1 tls_survives=1 hardware=0\n";
    return 0;
  } catch (const std::exception &error) {
    fixture.cleanupAllowed = true;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
