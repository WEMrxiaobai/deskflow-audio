/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "Diagnostic.h"

#include "common/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QSettings>

namespace deskflow::gui::diagnostic {

#ifdef Q_OS_WIN
namespace {
const auto kRunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const auto kStartupApprovedKey =
    QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run");
const auto kRunName = QStringLiteral("Deskflow");

QString startupCommand()
{
  return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}
}
#endif

bool startsAtLogin()
{
#ifdef Q_OS_WIN
  QSettings run(kRunKey, QSettings::NativeFormat);
  if (!run.contains(kRunName))
    return false;
  QSettings approved(kStartupApprovedKey, QSettings::NativeFormat);
  const auto state = approved.value(kRunName).toByteArray();
  return state.isEmpty() || (static_cast<unsigned char>(state.at(0)) & 1) == 0;
#else
  return false;
#endif
}

bool startupPointsToCurrentExecutable()
{
#ifdef Q_OS_WIN
  QSettings run(kRunKey, QSettings::NativeFormat);
  return run.value(kRunName).toString().compare(startupCommand(), Qt::CaseInsensitive) == 0;
#else
  return false;
#endif
}

bool setStartsAtLogin(bool enabled)
{
#ifdef Q_OS_WIN
  QSettings run(kRunKey, QSettings::NativeFormat);
  if (enabled)
    run.setValue(kRunName, startupCommand());
  else
    run.remove(kRunName);
  run.sync();
  if (run.status() != QSettings::NoError)
    return false;

  QSettings approved(kStartupApprovedKey, QSettings::NativeFormat);
  approved.remove(kRunName);
  approved.sync();
  return approved.status() == QSettings::NoError && startsAtLogin() == enabled &&
         (!enabled || startupPointsToCurrentExecutable());
#else
  return !enabled;
#endif
}

void restart()
{
  QString program = QCoreApplication::applicationFilePath();
  QStringList arguments = QCoreApplication::arguments();

  // look for and remove --reset option if found
  if (int resetIndex = arguments.indexOf("--reset"); resetIndex != -1)
    arguments.remove(resetIndex);

  qInfo("launching new process: %s", qPrintable(program));
  QProcess::startDetached(program, arguments);

  qDebug("exiting current process");
  QCoreApplication::exit();
}

bool clearSettings(bool enableRestart, bool clearStartup)
{
  if (clearStartup && !setStartsAtLogin(false)) {
    qCritical("could not clear Windows sign-in startup setting");
    return false;
  }
  qDebug("clearing settings");
  Settings::proxy().clear();
  Settings::clearState();

  // save but do not emit saving signal which will prevent the current state of
  // the app config and server configs from being applied.
  Settings::save(false);

  auto profileDir = QDir(Settings::settingsPath());
  qDebug("removing profile dir: %s", qPrintable(profileDir.absolutePath()));
  profileDir.removeRecursively();

#ifdef Q_OS_WIN
  if (Settings::isPortableMode()) {
    // make a new empty portable settings file
    if (profileDir.mkpath(Settings::settingsPath())) {
      QFile file(Settings::settingsFile());
      std::ignore = file.open(QIODevice::WriteOnly);
      file.write(" ", 1);
      file.close();
    }
  }
#endif

  if (enableRestart) {
    qDebug("restarting");
    restart();
  } else {
    qDebug("skipping restart");
  }
  return true;
}

} // namespace deskflow::gui::diagnostic
