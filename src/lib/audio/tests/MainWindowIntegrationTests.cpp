/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// REAL MainWindow smoke, isolated portable config, no core startup or audio.
#include "common/Constants.h"
#include "common/Settings.h"
#include "gui/MainWindow.h"
#include "gui/audio/PairedAudioControl.h"
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
#include <QUuid>
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
  // Do not call Settings::setSettingsFile: it instantiates/cleans the initial
  // settings first. A portable fixture must exist before MainWindow constructs.
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
      "[gui]\nstartCoreWithGui=false\nenableUpdateCheck=false\ncloseToTray=false\naudioListen=false\n"
      "windowGeometry=@Rect(0 0 500 197)\n[security]\ntlsEnabled=false\n"
      "[core]\nprocessMode=1\ncomputerName=audio-mainwindow-smoke\n"
  );
  config.close();
  DialogWatch dialogs;
  app.installEventFilter(&dialogs);
  int result = 5;
  {
    MainWindow main(QStringLiteral("deskflow-audio-smoke-") + QUuid::createUuid().toString(QUuid::Id128));
    main.show();
    QTimer::singleShot(1800, &main, [&] {
      auto *audio = main.findChild<AudioSharingWidget *>(QStringLiteral("audioSharingGroup"));
      auto *listen = main.findChild<QCheckBox *>(QStringLiteral("audioListenSwitch"));
      auto *stop = main.findChild<QPushButton *>(QStringLiteral("audioStopButton"));
      auto *status = main.findChild<QLabel *>(QStringLiteral("audioStatusLabel"));
      auto *control = audio ? audio->findChild<PairedAudioControl *>() : nullptr;
      const bool controls = audio && audio->isVisible() && listen && listen->isVisible() && listen->isEnabled() &&
                            !listen->isChecked() && stop && stop->isHidden() && status && status->isVisible() &&
                            !status->text().isEmpty() &&
                            !main.findChild<QPushButton *>(QStringLiteral("audioReceiveButton")) &&
                            !main.findChild<QPushButton *>(QStringLiteral("audioSendButton"));
      const bool fits = audio && main.rect().contains(audio->mapTo(&main, QPoint(0, 0))) &&
                        main.rect().contains(audio->mapTo(&main, QPoint(audio->width() - 1, audio->height() - 1)));
      const bool idle = control && control->peerIds().isEmpty() &&
                        !Settings::value(Settings::Gui::AutoStartCore).toBool() && !dialogs.seen &&
                        !QApplication::activeModalWidget();
      const bool isolated = Settings::settingsFile() == QFileInfo(path).absoluteFilePath();
      std::cout << "real_mainwindow_switch_only=" << controls << " fits_old_window_geometry=" << fits
                << " unstarted_core_idle=" << idle << " isolated_settings=" << isolated << " dialogs=" << dialogs.seen
                << '\n';
      main.grab().save(QApplication::applicationDirPath() + QStringLiteral("/integrated-mainwindow.png"));
      result = controls && fits && idle && isolated ? 0 : 6;
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
  // Remove only our CREATE_NEW fixture, never an existing installed config.
  if (!QFile::remove(path))
    return 8;
  return result;
}
