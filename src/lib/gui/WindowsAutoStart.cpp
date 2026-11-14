/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WindowsAutoStart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace deskflow::gui {

namespace {
// 固定值名让安装版和便携版共用一个登录入口；路径变化时由用户保存设置进行修复，禁止后台抢占。
const auto kValueName = QStringLiteral("Deskflow");
} // namespace

WindowsAutoStart::WindowsAutoStart(QString executable, QString registryPath)
    : m_executable(executable.isEmpty() ? QCoreApplication::applicationFilePath() : executable),
      m_registryPath(
          registryPath.isEmpty()
              ? QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run")
              : registryPath
      )
{
}

QString WindowsAutoStart::command() const
{
  // 始终引用完整 exe 路径，支持 Program Files、中文目录等；不依赖 Windows 登录时的工作目录。
  return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QDir::cleanPath(m_executable)));
}

WindowsAutoStart::State WindowsAutoStart::state() const
{
#ifdef Q_OS_WIN
  QSettings registry(m_registryPath, QSettings::NativeFormat);
  registry.setFallbacksEnabled(false);
  registry.sync();
  const auto registered = registry.contains(kValueName);
  const auto stored = registry.value(kValueName).toString();
  if (registry.status() != QSettings::NoError)
    return {false, false, QCoreApplication::translate("WindowsAutoStart", "Unable to read the Windows startup entry.")};
  return {registered, registered && stored == command(), {}, registry.isWritable()};
#else
  return {};
#endif
}

QString WindowsAutoStart::setEnabled(bool enabled) const
{
#ifdef Q_OS_WIN
  if (enabled) {
    const QFileInfo executable(m_executable);
    // Run 命令有长度限制；先校验再写入，避免留下勾选成功却无法启动的无效命令。
    if (!executable.isAbsolute() || !executable.isFile() ||
        executable.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0 ||
        m_executable.contains(QLatin1Char('"')) || m_executable.contains(QLatin1Char('\n')) ||
        m_executable.contains(QLatin1Char('\r')) || command().size() > 260) {
      return QCoreApplication::translate(
          "WindowsAutoStart", "The application path is invalid or too long for Windows startup."
      );
    }
  }

  QSettings registry(m_registryPath, QSettings::NativeFormat);
  registry.setFallbacksEnabled(false);
  if (enabled)
    registry.setValue(kValueName, command());
  else
    registry.remove(kValueName);
  registry.sync();
  if (registry.status() != QSettings::NoError)
    return QCoreApplication::translate("WindowsAutoStart", "Unable to save the Windows startup entry.");

  // 重新读取持久化结果；不能仅凭 setValue 没抛异常就告知用户保存成功。
  const auto saved = state();
  if (!saved.error.isEmpty())
    return saved.error;
  if (saved.registered != enabled || (enabled && !saved.currentExecutable))
    return QCoreApplication::translate("WindowsAutoStart", "The Windows startup entry could not be verified.");
  return {};
#else
  Q_UNUSED(enabled);
  return QCoreApplication::translate("WindowsAutoStart", "Windows startup is unavailable on this platform.");
#endif
}

} // namespace deskflow::gui
