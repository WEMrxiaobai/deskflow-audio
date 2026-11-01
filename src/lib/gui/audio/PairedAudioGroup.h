/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include "Session.h"
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <vector>

class PairedAudioControl;
// The original keyboard server brokers a single listener and two source routes.
// Only control messages cross the hub; each source sends SRTP directly to the listener.
class PairedAudioGroup final : public QObject
{
  Q_OBJECT
public:
  struct Source
  {
    QString id, name, status;
    std::shared_ptr<preview::SourcePlaybackControl> control;
    std::uint64_t decodedPackets = 0;
    std::size_t playbackSlot = 0;
  };
  PairedAudioGroup(PairedAudioControl *, const std::atomic<bool> *localPlayback, QObject *parent = nullptr);
  ~PairedAudioGroup() override;
  void setLocalState(bool listen, bool available);
  bool handleMessage(const QString &peer, const QJsonObject &);
  bool enabled() const;
  void shutdown();
  std::vector<Source> sources() const;
  void saveSourceControls();
  // Must precede negotiation. Synthetic capture; render=true opens only the output device.
  void setTestMode(bool render = false);
  std::shared_ptr<preview::SharedPlayback> playbackForTest() const;
Q_SIGNALS:
  void modeChanged(bool enabled);
  void statusChanged(const QString &);
  void senderChanged(bool sharing, bool overridden);
  void sourcesChanged();

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
