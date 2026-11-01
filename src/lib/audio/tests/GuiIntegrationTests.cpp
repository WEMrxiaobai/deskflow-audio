/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Isolated switch-only GUI smoke: no paired peers, audio devices, or user settings.
#include "common/Constants.h"
#include "common/Settings.h"
#include "gui/audio/PairedAudioControl.h"
#include "gui/config/ServerConfig.h"
#include "gui/core/CoreProcess.h"
#include "gui/widgets/AudioSharingWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

namespace {
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
} // namespace
int main(int argc, char **argv)
{
  QApplication::setApplicationName(kAppName);
  QApplication::setOrganizationName(QStringLiteral("DeskflowIntegrationTest"));
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  QTemporaryDir temporary;
  if (!temporary.isValid())
    return 1;
  qputenv("XDG_STATE_HOME", temporary.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
  // Settings::setSettingsFile first instantiates Settings, so it cannot safely
  // isolate the initial load. Create a portable fixture BEFORE any Settings call.
  const auto folder = QApplication::applicationDirPath() + QStringLiteral("/settings");
  if (!QDir().mkpath(folder))
    return 2;
  QLockFile lock(folder + QStringLiteral("/audio-smoke.lock"));
  if (!lock.tryLock(10000))
    return 3;
  const auto path = folder + QStringLiteral("/") + kAppName + QStringLiteral(".conf");
  QFile config(path);
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    std::cerr << "Refusing to overwrite an existing portable configuration\n";
    return 4;
  }
  config.write(
      "[gui]\naudioListen=false\nstartCoreWithGui=false\nenableUpdateCheck=false\n"
      "[security]\ntlsEnabled=false\n[core]\nprocessMode=1\ncomputerName=audio-switch-smoke\n"
  );
  config.close();
  DialogWatch dialogs;
  app.installEventFilter(&dialogs);
  int result = 5;
  {
    ServerConfig serverConfig;
    deskflow::gui::CoreProcess core(serverConfig);
    AudioSharingWidget widget;
    widget.attach(&core); // Unstarted core must never negotiate or open audio.
    widget.show();
    auto *listen = widget.findChild<QCheckBox *>(QStringLiteral("audioListenSwitch"));
    auto *stop = widget.findChild<QPushButton *>(QStringLiteral("audioStopButton"));
    auto *local = widget.findChild<QCheckBox *>(QStringLiteral("audioLocalPlaybackSwitch"));
    const bool localDefaultOff = local && !local->isChecked() && local->isHidden();
    bool localPersisted = false;
    if (local) {
      local->setChecked(true);
      const bool on = Settings::value(Settings::Gui::AudioLocalPlayback).toBool();
      {
        AudioSharingWidget restored;
        auto *saved = restored.findChild<QCheckBox *>(QStringLiteral("audioLocalPlaybackSwitch"));
        localPersisted = on && saved && saved->isChecked();
      }
      local->setChecked(false);
      localPersisted = localPersisted && !Settings::value(Settings::Gui::AudioLocalPlayback).toBool();
    }
    auto *status = widget.findChild<QLabel *>(QStringLiteral("audioStatusLabel"));
    auto *control = widget.findChild<PairedAudioControl *>();
    bool toggledOn = false, toggledOff = false;
    QTimer::singleShot(0, &widget, [&] {
      if (!listen)
        return;
      listen->click();
      toggledOn = listen->isChecked() && Settings::value(Settings::Gui::AudioListen).toBool();
      listen->click();
      toggledOff = !listen->isChecked() && !Settings::value(Settings::Gui::AudioListen).toBool();
    });
    QTimer::singleShot(1800, &widget, [&] {
      const bool controls = listen && listen->isVisible() && listen->isEnabled() && stop && stop->isHidden() &&
                            status && status->isVisible() && !status->text().isEmpty() &&
                            status->textFormat() == Qt::PlainText &&
                            !widget.findChild<QPushButton *>(QStringLiteral("audioReceiveButton")) &&
                            !widget.findChild<QPushButton *>(QStringLiteral("audioSendButton"));
      const bool idle = !core.isStarted() && control && control->peerIds().isEmpty() && !dialogs.seen &&
                        !QApplication::activeModalWidget();
      const bool isolated = Settings::settingsFile() == QFileInfo(path).absoluteFilePath();
      std::cout << "switch_only_controls=" << controls << " persisted_on=" << toggledOn
                << " persisted_off=" << toggledOff << " stopped_core_idle=" << idle << " isolated_settings=" << isolated
                << " dialogs=" << dialogs.seen << '\n';
      std::cout << "local_playback_default_off=" << localDefaultOff << " local_playback_persisted=" << localPersisted
                << '\n';
      result = controls && toggledOn && toggledOff && idle && isolated && localDefaultOff && localPersisted ? 0 : 6;
      app.quit();
    });
    QTimer::singleShot(6000, &app, [&] {
      result = 7;
      app.quit();
    });
    app.exec();
  }
  Settings::save();
  app.removeEventFilter(&dialogs);
  // Only the exact file atomically created above is removed; pre-existing files
  // are rejected, never overwritten or deleted.
  if (!QFile::remove(path))
    return 8;
  return result;
}
