/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "AudioSharingWidget.h"
#include "Session.h"
#include "common/Settings.h"
#include "gui/audio/AudioSessionWorker.h"
#include "gui/audio/PairedAudioControl.h"
#include "gui/core/CoreProcess.h"
#include <QCheckBox>
#include <QDebug>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QJsonObject offerJson(const preview::Invitation &offer)
{
  return {
      {"media", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(offer.mediaKey.data()), 30).toBase64())},
      {"control",
       QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(offer.controlKey.data()), 32).toBase64())},
      {"ssrc", QString::number(offer.ssrc)},
      {"expires", QString::number(offer.expiresUnixSeconds)},
      {"port", static_cast<int>(offer.port)}
  };
}
bool readOffer(const QJsonObject &json, const QString &address, preview::Invitation &offer)
{
  const auto media = QByteArray::fromBase64(json.value("media").toString().toLatin1());
  const auto control = QByteArray::fromBase64(json.value("control").toString().toLatin1());
  bool ssrcOk = false, expiresOk = false;
  offer.ssrc = json.value("ssrc").toString().toUInt(&ssrcOk);
  offer.expiresUnixSeconds = json.value("expires").toString().toULongLong(&expiresOk);
  const auto port = json.value("port").toInt();
  QHostAddress host(address);
  bool ipv4 = false;
  const auto ip = host.toIPv4Address(&ipv4);
  const auto now = preview::unixSeconds();
  // 邀请由接收电脑按其本地时间签发；发送电脑时钟稍慢时，看到的剩余时间会超过十分钟。
  // 与媒体层使用同一时钟余量，先检查到期再做无符号减法；仍拒绝过期和超出上限的邀请。
  if (media.size() != 30 || control.size() != 32 || !ssrcOk || !offer.ssrc || !expiresOk ||
      offer.expiresUnixSeconds <= now || offer.expiresUnixSeconds - now > preview::maxInvitationRemainingSeconds ||
      port < 1 || port > 65535 || !ipv4)
    return false;
  std::copy(media.begin(), media.end(), offer.mediaKey.begin());
  std::copy(control.begin(), control.end(), offer.controlKey.begin());
  offer.port = static_cast<std::uint16_t>(port);
  // Media destination is always the actual pinned TLS peer, never an offered IP.
  offer.addresses = {QHostAddress(ip).toString().toStdString()};
  return true;
}
} // namespace

// 冷却或清理期间最多保留一份可信邀请；替换、撤销和控件销毁均擦除密钥。
struct AudioSharingWidget::PendingOffer
{
  QString peer, session;
  preview::Invitation invitation;
  ~PendingOffer()
  {
    preview::erase(invitation);
  }
};

AudioSharingWidget::AudioSharingWidget(QWidget *parent)
    : QGroupBox(QStringLiteral("音频共享"), parent),
      m_control(new PairedAudioControl(this)),
      m_workerPoll(new QTimer(this))
{
  // 只有存在音频任务时才以 50ms 间隔取一次有界邮箱；清理期间保留轮询，禁止忙等。
  m_retryClock.start();
  m_workerPoll->setInterval(50);
  connect(m_workerPoll, &QTimer::timeout, this, &AudioSharingWidget::pollSession);
  setObjectName(QStringLiteral("audioSharingGroup"));
  auto *layout = new QVBoxLayout(this);
  m_listen = new QCheckBox(QStringLiteral("在本机收听其他电脑"), this);
  m_listen->setObjectName(QStringLiteral("audioListenSwitch"));
  m_listen->setChecked(Settings::value(Settings::Gui::AudioListen).toBool());
  auto *row = new QHBoxLayout;
  row->addWidget(m_listen);
  row->addStretch();
  m_stop = new QPushButton(QStringLiteral("停止共享本机声音"), this);
  m_stop->setObjectName(QStringLiteral("audioStopButton"));
  m_stop->setVisible(false);
  row->addWidget(m_stop);
  layout->addLayout(row);
  m_localPlayback = new QCheckBox(QStringLiteral("共享时本机也播放"), this);
  m_localPlayback->setObjectName(QStringLiteral("audioLocalPlaybackSwitch"));
  m_playLocally = Settings::value(Settings::Gui::AudioLocalPlayback).toBool();
  m_localPlayback->setChecked(m_playLocally);
  m_localPlayback->setToolTip(QStringLiteral("不勾选：共享连接成功后临时静音本机扬声器；停止共享后恢复。"));
  // The original collapsed MainWindow is fixed-size; reserve the sender row.
  auto localPlaybackSize = m_localPlayback->sizePolicy();
  localPlaybackSize.setRetainSizeWhenHidden(true);
  m_localPlayback->setSizePolicy(localPlaybackSize);
  m_localPlayback->setVisible(false);
  layout->addWidget(m_localPlayback);
  connect(m_localPlayback, &QCheckBox::toggled, this, [this](bool enabled) {
    m_playLocally = enabled;
    if (m_worker && m_running)
      m_worker->setLocalPlayback(enabled);
    Settings::setValue(Settings::Gui::AudioLocalPlayback, enabled);
    Settings::save();
  });
  m_status = new QLabel(QStringLiteral("键鼠配对后音频直接可用，无需再次配对。"), this);
  m_status->setObjectName(QStringLiteral("audioStatusLabel"));
  m_status->setWordWrap(true);
  m_status->setTextFormat(Qt::PlainText);
  m_status->setMinimumHeight(fontMetrics().lineSpacing() * 2);
  layout->addWidget(m_status);
  connect(m_listen, &QCheckBox::toggled, this, [this](bool enabled) {
    Settings::setValue(Settings::Gui::AudioListen, enabled);
    Settings::save();
    m_senderPaused = false;
    stopSession();
    advertise();
    reconcile();
  });
  connect(m_stop, &QPushButton::clicked, this, [this] {
    m_senderPaused = true;
    stopSession();
    advertise();
    m_status->setText(QStringLiteral("已停止共享本机声音 · 键鼠保持连接"));
  });
  connect(m_control, &PairedAudioControl::statusChanged, this, [this](const QString &text) {
    if (!m_running)
      m_status->setText(text);
  });
  connect(m_control, &PairedAudioControl::peerConnected, this, [this](const QString &peer, const QString &name) {
    m_peerNames[peer] = name;
    advertise();
  });
  connect(m_control, &PairedAudioControl::peerDisconnected, this, [this](const QString &peer) {
    m_peerNames.remove(peer);
    m_remoteListen.remove(peer);
    m_remoteAvailable.remove(peer);
    m_remoteRecovery.remove(peer);
    if (m_pendingOffer && m_pendingOffer->peer == peer)
      m_pendingOffer.reset();
    if (m_activePeer == peer)
      stopSession(false, "control-peer-disconnected");
    reconcile();
  });
  connect(m_control, &PairedAudioControl::messageReceived, this, &AudioSharingWidget::handleMessage);
}
AudioSharingWidget::~AudioSharingWidget()
{
  m_destroying = true;
  // Children outlive C++ members until QObject's base destructor. Prevent the
  // controller's shutdown signals from reaching already-destroyed member state.
  disconnect(m_control, nullptr, this, nullptr);
  m_control->configure(false, false, {});
  // 析构只发出停止请求。任务及策略原子变量由后台共享状态持有，不再访问控件。
  m_workerPoll->stop();
  m_worker.reset();
}
void AudioSharingWidget::attach(deskflow::gui::CoreProcess *core)
{
  m_core = core;
  connect(core, &deskflow::gui::CoreProcess::connectionStateChanged, this, [this] { updateConnection(); });
  connect(core, &deskflow::gui::CoreProcess::processStateChanged, this, [this](deskflow::core::ProcessState state) {
    if (state != deskflow::core::ProcessState::Started)
      m_connectedNames.clear();
    updateConnection();
  });
  connect(core, &deskflow::gui::CoreProcess::connectedClientsChanged, this, [this](const QStringList &names) {
    m_connectedNames = names;
    updateConnection();
  });
  auto *timer = new QTimer(this);
  timer->setInterval(1500);
  connect(timer, &QTimer::timeout, this, [this] {
    updateConnection();
    reconcile();
  });
  timer->start();
  QTimer::singleShot(0, this, [this] { updateConnection(); });
}
void AudioSharingWidget::updateConnection()
{
  if (!m_core)
    return;
  const bool server = m_core->mode() == Settings::CoreMode::Server;
  const bool ready =
      m_core->isStarted() &&
      (server ? !m_connectedNames.isEmpty() : m_core->connectionState() == deskflow::core::ConnectionState::Connected);
  if (!ready && m_running)
    stopSession(false, "keyboard-connection-unavailable");
  m_control->configure(server, ready, Settings::value(Settings::Client::RemoteHost).toString(), m_connectedNames);
  if (!ready && !m_running)
    m_status->setText(QStringLiteral("等待键鼠连接 · 配对成功后音频自动可用"));
}
void AudioSharingWidget::advertise()
{
  for (const auto &peer : m_control->peerIds())
    // 可选能力字段不改变旧对端协议；只有双方均声明恢复能力时才走会话内重同步。
    m_control->send(
        peer, {{"type", "state"}, {"listen", m_listen->isChecked()}, {"available", !m_senderPaused}, {"recovery", true}}
    );
}
bool AudioSharingWidget::shouldReceive(const QString &peer) const
{
  return m_listen->isChecked() && m_remoteListen.contains(peer) && m_remoteAvailable.value(peer) &&
         (!m_remoteListen.value(peer) || m_control->isServer());
}
bool AudioSharingWidget::shouldSend(const QString &peer) const
{
  return !m_senderPaused && m_remoteListen.value(peer) && (!m_listen->isChecked() || !m_control->isServer());
}
void AudioSharingWidget::reconcile()
{
  if (!canStartSession())
    return;
  auto peers = m_control->peerIds();
  // 保留一份等待中的邀请，避免发送端在冷却时丢弃邀请，让接收端空等整个握手超时。
  // 启动前重新验证连接、意图和凭据期限，旧连接或已取消的邀请不能恢复音频。
  if (m_pendingOffer) {
    const auto &pending = *m_pendingOffer;
    if (peers.contains(pending.peer) && shouldSend(pending.peer) &&
        pending.invitation.expiresUnixSeconds > preview::unixSeconds()) {
      startSession(pending.peer, false, pending.session, &pending.invitation);
      if (m_running)
        m_pendingOffer.reset();
      return;
    }
    m_pendingOffer.reset();
  }
  peers.sort();
  for (const auto &peer : peers)
    if (shouldReceive(peer)) {
      startSession(peer, true, QUuid::createUuid().toString(QUuid::Id128));
      return;
    }
  if (peers.isEmpty())
    return;
  m_status->setText(
      m_listen->isChecked() ? QStringLiteral("已配对 · 等待对方音频就绪")
                            : QStringLiteral("已配对 · 在插耳机的电脑打开收听开关即可")
  );
}
void AudioSharingWidget::handleMessage(const QString &peer, const QJsonObject &message)
{
  const auto type = message.value("type").toString();
  if (type == "state") {
    if (!message.value("listen").isBool() || !message.value("available").isBool())
      return;
    m_remoteListen[peer] = message.value("listen").toBool();
    m_remoteAvailable[peer] = message.value("available").toBool();
    m_remoteRecovery[peer] = message.value("recovery").toBool(false);
    if (m_activePeer == peer && m_running && !(m_receive ? shouldReceive(peer) : shouldSend(peer)))
      stopSession();
    reconcile();
    return;
  }
  const auto session = message.value("session").toString();
  if (session.size() != 32)
    return;
  if (type == "recover" || type == "resume") {
    // TLS 已核验身份；这里再核验当前会话、方向和整数范围，旧会话消息不能影响新 worker。
    if (!m_running || peer != m_activePeer || session != m_session || !m_recovery || (type == "resume") != m_receive)
      return;
    bool requestOk = false;
    const auto request = message.value("request").toString().toULongLong(&requestOk);
    if (!requestOk || !request)
      return;
    preview::RecoveryMessage recovery{type == "resume", request};
    if (recovery.response) {
      // 旧回复未带此字段时仍解释为已有媒体；新对端可以明确报告尚未产生首包。
      const auto hasMedia = message.value("hasMedia");
      if (!hasMedia.isUndefined() && !hasMedia.isBool())
        return;
      recovery.hasMedia = hasMedia.toBool(true);
      const auto value = message.value("sequence");
      const auto sequence = value.toInt(-1);
      bool timestampOk = false;
      recovery.timestamp = message.value("timestamp").toString().toUInt(&timestampOk);
      if (!value.isDouble() || sequence < 0 || sequence > 65535 || value.toDouble() != sequence || !timestampOk)
        return;
      recovery.sequence = static_cast<std::uint16_t>(sequence);
    }
    m_recovery->post(recovery);
    return;
  }
  if (type == "stop") {
    if (m_pendingOffer && peer == m_pendingOffer->peer && session == m_pendingOffer->session)
      m_pendingOffer.reset();
    if (peer == m_activePeer && session == m_session)
      stopSession(false, "peer-stop");
    return;
  }
  if (type == "offer" && shouldSend(peer) && !m_running) {
    preview::Invitation offer;
    if (readOffer(message.value("offer").toObject(), m_control->peerAddress(peer), offer)) {
      m_pendingOffer = std::make_unique<PendingOffer>();
      m_pendingOffer->peer = peer;
      m_pendingOffer->session = session;
      m_pendingOffer->invitation = offer;
      reconcile();
    } else {
      // 只记录拒绝原因类别，不输出邀请、媒体密钥或完整控制消息，便于区分未发 UDP 与网络丢包。
      qWarning("audio-session reason=invalid-audio-offer 音频邀请无效或已过期，等待重新协商。");
    }
    preview::erase(offer);
  }
}
// 明确记录停止来源；授权失效立即取消音频，耗时释放留给后台，不影响键鼠 core。
void AudioSharingWidget::stopSession(bool inform, const char *reason)
{
  // 用户关开关、停止共享或门控失效时，同时取消尚未启动的邀请。
  m_pendingOffer.reset();
  if (!m_running)
    return;
  qInfo("audio-session role=%s reason=%s", m_receive ? "receiver" : "sender", reason);
  if (inform)
    m_control->send(m_activePeer, {{"type", "stop"}, {"session", m_session}});
  m_worker->requestStop();
  m_recovery.reset();
  m_running = false;
  m_activePeer.clear();
  m_session.clear();
  m_stop->setVisible(false);
  m_localPlayback->setVisible(false);
  // 停止后仍保留唯一任务句柄，直到清理完成；频繁切换开关不能叠加声卡和守护进程。
  m_nextStartMs = m_retryClock.elapsed() + 2000;
}

// 所有入口共用门槛：定时检查、远端 offer、状态变化都不能绕过清理屏障及两秒限频。
bool AudioSharingWidget::canStartSession() const
{
  return !m_destroying && !m_running && !m_worker && m_retryClock.elapsed() >= m_nextStartMs;
}

void AudioSharingWidget::startSession(
    const QString &peer, bool receive, const QString &session, const preview::Invitation *offer
)
{
  if (!canStartSession())
    return;
  m_nextStartMs = m_retryClock.elapsed() + 2000;
  m_activePeer = peer;
  m_session = session;
  m_receive = receive;
  m_localPlaybackOverridden = false;
  m_localPlayback->setEnabled(true);
  m_localPlayback->setToolTip(QStringLiteral("不勾选：共享连接成功后临时静音本机扬声器；停止共享后恢复。"));
  m_running = true;
  m_stop->setVisible(!receive);
  m_localPlayback->setVisible(!receive);
  m_status->setText(receive ? QStringLiteral("正在自动连接并收听…") : QStringLiteral("正在向已配对电脑共享系统声音…"));
  preview::SessionOptions options;
  options.receive = receive;
  if (m_remoteRecovery.value(peer)) {
    m_recovery = std::make_shared<preview::SessionRecovery>();
    options.recovery = m_recovery;
  }
  if (offer)
    options.invitation = *offer;
  try {
    // 后台任务接管所有回调和原子标志；这里不把 this 或任何 QObject 指针交给线程。
    m_worker = std::make_unique<AudioSessionWorker>(std::move(options), m_playLocally);
    m_workerPoll->start();
  } catch (const std::exception &) {
    m_running = false;
    m_recovery.reset();
    m_activePeer.clear();
    m_session.clear();
    m_stop->setVisible(false);
    m_localPlayback->setVisible(false);
    m_status->setText(QStringLiteral("音频暂不可用，正在自动重试"));
    QTimer::singleShot(2000, this, [this] { reconcile(); });
  }
  // std::move 不会清空数组型密钥；启动成功及异常路径均清除当前栈上的邀请副本。
  if (options.invitation)
    preview::erase(*options.invitation);
}

// 邮箱和 UI 只在 Qt 线程交接。主动停止后丢弃全部旧结果，避免恢复、邀请或状态重新生效。
void AudioSharingWidget::pollSession()
{
  if (!m_worker)
    return;
  auto update = m_worker->takeUpdate();
  // 日志不依赖会话仍处于运行态，主动停止后的最终诊断也可以输出；数量固定上限为八。
  for (const auto &event : update.logs) {
    if (event.state == preview::SessionState::Failed)
      qWarning().noquote() << "audio-session reason=" + QString::fromStdString(event.reason)
                           << QString::fromStdString(event.message);
    else
      qInfo().noquote() << QString::fromStdString(event.message);
  }
  if (!m_running) {
    if (update.finished) {
      m_worker.reset();
      m_workerPoll->stop();
      const auto wait = std::max<qint64>(0, m_nextStartMs - m_retryClock.elapsed());
      QTimer::singleShot(static_cast<int>(wait), this, [this] { reconcile(); });
    }
    return;
  }
  if (update.offer)
    m_control->send(m_activePeer, {{"type", "offer"}, {"session", m_session}, {"offer", offerJson(*update.offer)}});
  // send 可能同步发现控制连接失效并触发 stopSession，后续旧结果不得再更新界面或发恢复。
  if (!m_running)
    return;
  if (update.recovery) {
    const auto &recovery = *update.recovery;
    QJsonObject message{
        {"type", recovery.response ? "resume" : "recover"},
        {"session", m_session},
        {"request", QString::number(recovery.request)}
    };
    if (recovery.response) {
      message.insert("hasMedia", recovery.hasMedia);
      message.insert("sequence", static_cast<int>(recovery.sequence));
      message.insert("timestamp", QString::number(recovery.timestamp));
    }
    m_control->send(m_activePeer, message);
  }
  if (!m_running)
    return;
  if (update.playbackOverridden && !m_localPlaybackOverridden) {
    m_localPlaybackOverridden = true;
    m_localPlayback->setEnabled(false);
    m_localPlayback->setToolTip(
        QStringLiteral("本机声音已由系统或用户调整；本次共享尊重该设置，下次共享恢复使用此选项。")
    );
    m_status->setText(QStringLiteral("正在共享系统声音 · 本机播放遵循系统当前设置"));
  }
  if (update.status) {
    const auto state = update.status->state;
    if (state == preview::SessionState::Recovering)
      m_status->setText(QStringLiteral("声音暂时中断，正在恢复…"));
    else if (state == preview::SessionState::WaitingForMedia)
      m_status->setText(QStringLiteral("对方暂无系统声音，等待播放…"));
    else if (!m_localPlaybackOverridden)
      m_status->setText(
          m_receive ? QStringLiteral("正在收听已配对电脑的声音")
                    : QStringLiteral("正在向「%1」共享系统声音")
                          .arg(m_peerNames.value(m_activePeer, QStringLiteral("已配对电脑")))
      );
  }
  if (!update.finished)
    return;
  // 完成标志在后台资源全部释放后才发布；普通失败等待两秒，正常换密钥可立即重建音频。
  const auto peer = m_activePeer, session = m_session;
  const bool renewing = update.renewing && update.failure.empty();
  const auto delay = renewing ? 0 : 2000;
  // 先设置重试门槛再发送 STOP，防止同步断开信号中的 reconcile 越过冷却期。
  m_nextStartMs = m_retryClock.elapsed() + delay;
  m_worker.reset();
  m_workerPoll->stop();
  m_running = false;
  m_recovery.reset();
  m_stop->setVisible(false);
  m_localPlayback->setVisible(false);
  m_activePeer.clear();
  m_session.clear();
  m_status->setText(
      renewing                 ? QStringLiteral("正在更新音频连接…")
      : update.failure.empty() ? QStringLiteral("音频已停止 · 键鼠保持连接")
                               : QStringLiteral("%1 · 正在自动重试").arg(QString::fromStdString(update.failure))
  );
  QTimer::singleShot(delay, this, [this] { reconcile(); });
  // 最后才发送；同步断开信号可能协调另一对端，旧会话不能覆盖随后创建的新会话状态。
  m_control->send(peer, {{"type", "stop"}, {"session", session}});
}
