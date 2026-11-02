/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/Constants.h"
#include "gui/config/ServerConfig.h"
#include "gui/dialogs/SettingsDialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv)
{
  QApplication::setApplicationName(kAppName);
  QApplication app(argc, argv);
  const auto folder = QApplication::applicationDirPath() + QStringLiteral("/settings");
  if (!QDir().mkpath(folder))
    return 1;
  QFile config(folder + QStringLiteral("/") + kAppName + QStringLiteral(".conf"));
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return 2;
  config.write("[gui]\nstartCoreWithGui=false\nenableUpdateCheck=false\n[security]\ntlsEnabled=false\n");
  config.close();
  QTimer closeMessage;
  QObject::connect(&closeMessage, &QTimer::timeout, [] {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QMessageBox *>(widget); dialog && dialog->isVisible())
        dialog->reject();
    }
  });
  closeMessage.start(100);
  int result = 0;
  {
    ServerConfig server;
    auto check = [&server](bool otherChange, bool writeSucceeds) {
      int writes = 0;
      bool writtenValue = false;
      SettingsDialog dialog(nullptr, server, [&](bool enabled) {
        ++writes;
        writtenValue = enabled;
        return writeSucceeds;
      });
      auto *startup = dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"));
      auto *port = dialog.findChild<QSpinBox *>(QStringLiteral("sbPort"));
      auto *buttons = dialog.findChild<QDialogButtonBox *>();
      if (!startup || !port || !buttons)
        return false;
      const bool desired = !startup->isChecked();
      startup->setChecked(desired);
      if (otherChange)
        port->setValue(port->value() + 1);
      buttons->button(QDialogButtonBox::Save)->click();
      return writes == 1 && writtenValue == desired &&
             (writeSucceeds ? dialog.result() == QDialog::Accepted && dialog.startupOnlyChange() == !otherChange
                            : dialog.result() != QDialog::Accepted && !dialog.startupOnlyChange());
    };
    const bool startupOnly = check(false, true);
    const bool mixed = check(true, true);
    const bool writeFailure = check(false, false);
    std::cout << "startup_only=" << startupOnly << " mixed_settings=" << mixed
              << " write_failure=" << writeFailure << '\n';
    if (!startupOnly || !mixed || !writeFailure)
      result = 3;
  }
  QFile::remove(config.fileName());
  return result;
}
