/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

namespace deskflow::gui {

/// Windows 登录启动项管理器：只管理当前用户的 Deskflow GUI，不改变服务、键鼠或音频状态。
class WindowsAutoStart
{
public:
  struct State
  {
    bool registered = false;
    bool currentExecutable = false;
    QString error;
    // 启动项权限独立于应用配置权限；读取成功但不可写时仍展示实际注册状态。
    bool writable = false;
  };

  /// 默认使用真实应用路径和 HKCU Run；测试可注入独立注册表子键，避免改动系统启动项。
  explicit WindowsAutoStart(QString executable = {}, QString registryPath = {});

  /// 读取实际注册值，而非配置文件中的意愿值；注册成功仍可能被 Windows 启动应用设置禁用。
  State state() const;

  /// 显式保存时才写入/删除自己的值，并同步、回读校验；失败返回可展示的错误原因。
  QString setEnabled(bool enabled) const;

private:
  QString command() const;

  QString m_executable;
  QString m_registryPath;
};

} // namespace deskflow::gui
