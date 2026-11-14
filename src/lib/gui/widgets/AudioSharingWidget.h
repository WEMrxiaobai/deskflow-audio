/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QElapsedTimer>
#include <QGroupBox>
#include <QHash>
#include <QJsonObject>
#include <memory>
class QLabel;
class QCheckBox;
class QPushButton;
class PairedAudioControl;
class QTimer;
class AudioSessionWorker;
namespace deskflow::gui {
class CoreProcess;
}
namespace preview {
struct Invitation;
class SessionRecovery;
} // namespace preview

// Audio reuses Deskflow's existing certificate and role-specific trust store.
// No audio-only pairing, authorization dialog, or invitation files.
class AudioSharingWidget : public QGroupBox
{
  Q_OBJECT
public:
  explicit AudioSharingWidget(QWidget *parent = nullptr);
  ~AudioSharingWidget() override;
  void attach(deskflow::gui::CoreProcess *core);

private:
  struct PendingOffer;
  void updateConnection();
  void advertise();
  void reconcile();
  void handleMessage(const QString &peer, const QJsonObject &message);
  void
  startSession(const QString &peer, bool receive, const QString &session, const preview::Invitation *offer = nullptr);
  void stopSession(bool inform = true, const char *reason = "user-stop");
  // 定时消费固定容量邮箱；后台清理完成后才允许创建下一次音频会话。
  void pollSession();
  bool canStartSession() const;
  bool shouldReceive(const QString &peer) const;
  bool shouldSend(const QString &peer) const;
  QCheckBox *m_listen;
  QCheckBox *m_localPlayback;
  QPushButton *m_stop;
  QLabel *m_status;
  PairedAudioControl *m_control;
  deskflow::gui::CoreProcess *m_core = nullptr;
  QHash<QString, QString> m_peerNames;
  QHash<QString, bool> m_remoteListen;
  QHash<QString, bool> m_remoteAvailable;
  QHash<QString, bool> m_remoteRecovery;
  QStringList m_connectedNames;
  QString m_activePeer, m_session;
  bool m_receive = false, m_running = false, m_destroying = false, m_senderPaused = false;
  bool m_localPlaybackOverridden = false;
  bool m_playLocally = false;
  std::unique_ptr<AudioSessionWorker> m_worker;
  QTimer *m_workerPoll;
  QElapsedTimer m_retryClock;
  qint64 m_nextStartMs = 0;
  std::shared_ptr<preview::SessionRecovery> m_recovery;
  std::unique_ptr<PendingOffer> m_pendingOffer;
  friend class AudioSharingWidgetTestAccess;
};
