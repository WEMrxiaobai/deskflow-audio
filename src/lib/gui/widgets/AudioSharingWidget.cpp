/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "AudioSharingWidget.h"
#include "Session.h"
#include "common/Settings.h"
#include "gui/audio/PairedAudioControl.h"
#include "gui/audio/PairedAudioGroup.h"
#include "gui/core/CoreProcess.h"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
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
  if (media.size() != 30 || control.size() != 32 || !ssrcOk || !offer.ssrc || !expiresOk ||
      offer.expiresUnixSeconds <= now || offer.expiresUnixSeconds > now + 600 || port < 1 || port > 65535 || !ipv4)
    return false;
  std::copy(media.begin(), media.end(), offer.mediaKey.begin());
  std::copy(control.begin(), control.end(), offer.controlKey.begin());
  offer.port = static_cast<std::uint16_t>(port);
  // Media destination is always the actual pinned TLS peer, never an offered IP.
  offer.addresses = {QHostAddress(ip).toString().toStdString()};
  return true;
}
} // namespace

AudioSharingWidget::AudioSharingWidget(QWidget *parent)
    : QGroupBox(QStringLiteral("音频共享"), parent),
      m_control(new PairedAudioControl(this))
{
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
  m_localPlayback->setChecked(m_playLocally.load());
  m_localPlayback->setToolTip(QStringLiteral("不勾选：共享连接成功后临时静音本机扬声器；停止共享后恢复。"));
  // The original collapsed MainWindow is fixed-size; reserve the sender row.
  auto localPlaybackSize = m_localPlayback->sizePolicy();
  localPlaybackSize.setRetainSizeWhenHidden(true);
  m_localPlayback->setSizePolicy(localPlaybackSize);
  m_localPlayback->setVisible(false);
  layout->addWidget(m_localPlayback);
  connect(m_localPlayback, &QCheckBox::toggled, this, [this](bool enabled) {
    m_playLocally.store(enabled);
    Settings::setValue(Settings::Gui::AudioLocalPlayback, enabled);
    Settings::save();
  });
  m_status = new QLabel(QStringLiteral("键鼠配对后音频直接可用，无需再次配对。"), this);
  m_status->setObjectName(QStringLiteral("audioStatusLabel"));
  m_status->setWordWrap(true);
  m_status->setTextFormat(Qt::PlainText);
  m_status->setMinimumHeight(fontMetrics().lineSpacing() * 2);
  layout->addWidget(m_status);
  m_group = new PairedAudioGroup(m_control, &m_playLocally, this);
  for (std::size_t i = 0; i < m_sourceRows.size(); ++i) {
    auto &source = m_sourceRows[i];
    source.container = new QWidget(this);
    source.container->setObjectName(QStringLiteral("audioSourceRow%1").arg(i));
    auto *sourceLayout = new QHBoxLayout(source.container);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    source.name = new QLabel(source.container);
    source.name->setTextFormat(Qt::PlainText);
    source.name->setMinimumWidth(130);
    source.volume = new QSlider(Qt::Horizontal, source.container);
    source.volume->setObjectName(QStringLiteral("audioSourceVolume%1").arg(i));
    source.volume->setRange(0, 100);
    source.volume->setAccessibleName(QStringLiteral("远端来源音量"));
    source.mute = new QCheckBox(QStringLiteral("静音"), source.container);
    source.mute->setObjectName(QStringLiteral("audioSourceMute%1").arg(i));
    sourceLayout->addWidget(source.name);
    sourceLayout->addWidget(source.volume, 1);
    sourceLayout->addWidget(source.mute);
    auto policy = source.container->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    source.container->setSizePolicy(policy);
    source.container->hide();
    layout->addWidget(source.container);
    connect(source.volume, &QSlider::valueChanged, this, [this, i](int value) {
      if (auto control = m_sourceRows[i].control) {
        control->volume = float(value) / 100;
        m_group->saveSourceControls();
      }
    });
    connect(source.mute, &QCheckBox::toggled, this, [this, i](bool value) {
      if (auto control = m_sourceRows[i].control) {
        control->muted = value;
        m_group->saveSourceControls();
      }
    });
  }
  connect(m_group, &PairedAudioGroup::modeChanged, this, [this](bool enabled) {
    if (enabled)
      stopSession();
    refreshSources();
    if (!enabled)
      reconcile();
  });
  connect(m_group, &PairedAudioGroup::statusChanged, m_status, &QLabel::setText);
  connect(m_group, &PairedAudioGroup::sourcesChanged, this, &AudioSharingWidget::refreshSources);
  connect(m_group, &PairedAudioGroup::senderChanged, this, [this](bool sharing, bool overridden) {
    m_stop->setVisible(sharing);
    m_localPlayback->setVisible(sharing);
    m_localPlayback->setEnabled(!overridden);
    m_localPlayback->setToolTip(
        overridden ? QStringLiteral("本机声音已由系统或用户调整；本次共享尊重该设置，下次共享恢复使用此选项。")
                   : QStringLiteral("不勾选：共享连接成功后临时静音本机扬声器；停止共享后恢复。")
    );
  });
  connect(m_listen, &QCheckBox::toggled, this, [this](bool enabled) {
    Settings::setValue(Settings::Gui::AudioListen, enabled);
    Settings::save();
    m_senderPaused = false;
    if (!m_group->enabled())
      stopSession();
    advertise();
    reconcile();
  });
  connect(m_stop, &QPushButton::clicked, this, [this] {
    m_senderPaused = true;
    if (!m_group->enabled())
      stopSession();
    advertise();
    m_status->setText(QStringLiteral("已停止共享本机声音 · 键鼠保持连接"));
  });
  connect(m_control, &PairedAudioControl::statusChanged, this, [this](const QString &text) {
    if (!m_running && !m_group->enabled())
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
    if (m_activePeer == peer)
      stopSession(false);
    reconcile();
  });
  connect(m_control, &PairedAudioControl::messageReceived, this, &AudioSharingWidget::handleMessage);
}
AudioSharingWidget::~AudioSharingWidget()
{
  m_destroying = true;
  disconnect(m_group, nullptr, this, nullptr);
  m_group->shutdown();
  // Children outlive C++ members until QObject's base destructor. Prevent the
  // controller's shutdown signals from reaching already-destroyed member state.
  disconnect(m_control, nullptr, this, nullptr);
  m_control->configure(false, false, {});
  m_stopRequested = true;
  if (m_worker.joinable())
    m_worker.join();
  m_localOutputState.reset();
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
  if (!ready && (m_running || m_localOutputState))
    stopSession(false);
  m_control->configure(server, ready, Settings::value(Settings::Client::RemoteHost).toString(), m_connectedNames);
  if (!ready && !m_running)
    m_status->setText(QStringLiteral("等待键鼠连接 · 配对成功后音频自动可用"));
}
void AudioSharingWidget::advertise()
{
  m_group->setLocalState(m_listen->isChecked(), !m_senderPaused);
  for (const auto &peer : m_control->peerIds())
    m_control->send(
        peer,
        {{"type", "state"}, {"listen", m_listen->isChecked()}, {"available", !m_senderPaused}, {"audioVersion", 2}}
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
  if (m_destroying || m_group->enabled() || m_running || (m_retryClock.isValid() && m_retryClock.elapsed() < 2000))
    return;
  auto peers = m_control->peerIds();
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
  if (message.value("type") == "state" && message.value("listen").isBool() && message.value("available").isBool()) {
    // Keep fallback state even while the hub coordinates a multi-source group.
    m_remoteListen[peer] = message.value("listen").toBool();
    m_remoteAvailable[peer] = message.value("available").toBool();
  }
  if (m_group->handleMessage(peer, message))
    return;
  const auto type = message.value("type").toString();
  if (type == "state") {
    if (!message.value("listen").isBool() || !message.value("available").isBool())
      return;
    m_remoteListen[peer] = message.value("listen").toBool();
    m_remoteAvailable[peer] = message.value("available").toBool();
    if (m_activePeer == peer && !(m_receive ? shouldReceive(peer) : shouldSend(peer)))
      stopSession();
    reconcile();
    return;
  }
  const auto session = message.value("session").toString();
  if (session.size() != 32)
    return;
  if (type == "stop") {
    if (peer == m_activePeer && session == m_session)
      stopSession(false, message.value("retry").toBool());
    return;
  }
  if (type == "offer" && shouldSend(peer) &&
      (!m_running || (m_activePeer == peer && !m_receive && m_session != session))) {
    preview::Invitation offer;
    if (readOffer(message.value("offer").toObject(), m_control->peerAddress(peer), offer)) {
      // A fresh authenticated offer supersedes this peer's old sending worker.
      // Preserve mute until the replacement has connected, avoiding a local burst.
      stopSession(false, m_activePeer == peer && !m_receive);
      startSession(peer, false, session, &offer);
    }
    preview::erase(offer);
  }
}
void AudioSharingWidget::refreshSources()
{
  const auto sources = m_group->sources();
  for (std::size_t i = 0; i < m_sourceRows.size(); ++i) {
    auto &row = m_sourceRows[i];
    row.control = i < sources.size() ? sources[i].control : nullptr;
    row.container->setVisible(bool(row.control));
    if (!row.control)
      continue;
    const auto label = sources[i].name + QStringLiteral(" · ") + sources[i].status;
    row.name->setText(row.name->fontMetrics().elidedText(label, Qt::ElideRight, 210));
    row.name->setToolTip(label);
    const QSignalBlocker volumeBlock(row.volume), muteBlock(row.mute);
    row.volume->setValue(qRound(row.control->volume.load() * 100));
    row.mute->setChecked(row.control->muted.load());
  }
}
void AudioSharingWidget::stopSession(bool inform, bool retry)
{
  if (inform && !m_session.isEmpty())
    m_control->send(m_activePeer, {{"type", "stop"}, {"session", m_session}, {"retry", retry}});
  ++m_generation;
  m_stopRequested = true;
  if (m_worker.joinable())
    m_worker.join();
  m_running = false;
  if (retry) {
    m_retryClock.start();
    m_status->setText(QStringLiteral("音频连接中断，正在自动重试 · 重连期间保持本机播放设置"));
  } else {
    m_retryClock.invalidate();
    m_localOutputState.reset();
    m_activePeer.clear();
    m_session.clear();
  }
  m_stop->setVisible(retry && bool(m_localOutputState));
  if (m_localPlayback)
    m_localPlayback->setVisible(false);
}
void AudioSharingWidget::startSession(
    const QString &peer, bool receive, const QString &session, const preview::Invitation *offer
)
{
  if (m_worker.joinable())
    m_worker.join();
  m_stopRequested = false;
  m_activePeer = peer;
  m_session = session;
  m_receive = receive;
  m_localPlaybackOverridden = false;
  m_localPlayback->setEnabled(true);
  m_running = true;
  m_retryClock.invalidate();
  const auto generation = ++m_generation;
  m_stop->setVisible(!receive);
  m_localPlayback->setVisible(!receive);
  m_status->setText(receive ? QStringLiteral("正在自动连接并收听…") : QStringLiteral("正在向已配对电脑共享系统声音…"));
  auto ownedOptions =
      std::shared_ptr<preview::SessionOptions>(new preview::SessionOptions, [](preview::SessionOptions *options) {
        if (options->invitation)
          preview::erase(*options->invitation);
        delete options;
      });
  auto &options = *ownedOptions;
  options.receive = receive;
  options.controlOwnsLifetime = true;
  if (!receive) {
    options.localPlayback = &m_playLocally;
    if (!m_localOutputState)
      m_localOutputState = preview::createLocalOutputState();
    options.localOutputState = m_localOutputState;
  }
  if (offer)
    options.invitation = *offer;
  if (receive)
    options.onReady = [this, peer, session, generation](const preview::Invitation &value) {
      const auto payload = offerJson(value);
      QMetaObject::invokeMethod(
          this,
          [this, peer, session, generation, payload] {
            if (generation == m_generation)
              m_control->send(peer, {{"type", "offer"}, {"session", session}, {"offer", payload}});
          },
          Qt::QueuedConnection
      );
    };
  try {
    m_worker = std::thread([this, ownedOptions, peer, session, generation, receive]() mutable {
      auto &options = *ownedOptions;
      QString failure;
      try {
        preview::runSession(options, m_stopRequested, [this, generation, receive](const preview::SessionEvent &event) {
          if (event.state == preview::SessionState::LocalPlaybackOverridden)
            QMetaObject::invokeMethod(
                this,
                [this, generation] {
                  if (generation != m_generation)
                    return;
                  m_localPlaybackOverridden = true;
                  m_localPlayback->setEnabled(false);
                  m_localPlayback->setToolTip(
                      QStringLiteral("本机声音已由系统或用户调整；本次共享尊重该设置，下次共享恢复使用此选项。")
                  );
                  m_status->setText(QStringLiteral("正在共享系统声音 · 本机播放遵循系统当前设置"));
                },
                Qt::QueuedConnection
            );
          if (event.state == preview::SessionState::Connected)
            QMetaObject::invokeMethod(
                this,
                [this, generation, receive] {
                  if (generation == m_generation && !m_localPlaybackOverridden)
                    m_status->setText(
                        receive ? QStringLiteral("正在收听已配对电脑的声音")
                                : QStringLiteral("正在向「%1」共享系统声音")
                                      .arg(m_peerNames.value(m_activePeer, QStringLiteral("已配对电脑")))
                    );
                },
                Qt::QueuedConnection
            );
        });
      } catch (const std::exception &error) {
        failure = QString::fromUtf8(error.what());
      }
      if (options.invitation)
        preview::erase(*options.invitation);
      QMetaObject::invokeMethod(
          this,
          [this, peer, session, generation, failure] {
            if (generation != m_generation)
              return;
            if (m_worker.joinable())
              m_worker.join();
            m_running = false;
            const bool retry = !failure.isEmpty();
            m_retryClock.start();
            m_stop->setVisible(retry && bool(m_localOutputState));
            if (m_localPlayback)
              m_localPlayback->setVisible(false);
            if (!retry) {
              m_localOutputState.reset();
              m_activePeer.clear();
              m_session.clear();
            }
            m_control->send(peer, {{"type", "stop"}, {"session", session}, {"retry", retry}});
            m_status->setText(
                failure.isEmpty() ? QStringLiteral("音频已停止 · 键鼠保持连接")
                                  : QStringLiteral("%1 · 正在自动重试").arg(failure)
            );
            QTimer::singleShot(2000, this, [this] { reconcile(); });
          },
          Qt::QueuedConnection
      );
    });
  } catch (const std::exception &) {
    m_running = false;
    m_retryClock.start();
    m_stop->setVisible(bool(m_localOutputState));
    if (m_localPlayback)
      m_localPlayback->setVisible(false);
    m_status->setText(QStringLiteral("音频暂不可用，正在自动重试"));
    m_control->send(peer, {{"type", "stop"}, {"session", session}, {"retry", true}});
    QTimer::singleShot(2000, this, [this] { reconcile(); });
  }
}
