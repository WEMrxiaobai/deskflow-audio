/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QGroupBox>
#include <QHash>
#include <QJsonObject>
#include <atomic>
#include <thread>
class QLabel;
class QCheckBox;
class QPushButton;
class PairedAudioControl;
namespace deskflow::gui {
class CoreProcess;
}
namespace preview {
struct Invitation;
}

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
  void updateConnection();
  void advertise();
  void reconcile();
  void handleMessage(const QString &peer, const QJsonObject &message);
  void
  startSession(const QString &peer, bool receive, const QString &session, const preview::Invitation *offer = nullptr);
  void stopSession(bool inform = true);
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
  QStringList m_connectedNames;
  QString m_activePeer, m_session;
  bool m_receive = false, m_running = false, m_destroying = false, m_senderPaused = false;
  bool m_localPlaybackOverridden = false;
  std::atomic<bool> m_stopRequested{false};
  std::atomic<bool> m_playLocally{false};
  std::thread m_worker;
  quint64 m_generation = 0;
};
