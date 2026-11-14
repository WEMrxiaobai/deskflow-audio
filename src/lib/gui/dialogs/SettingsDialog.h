/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once
#include <QDialog>

#include "gui/WindowsAutoStart.h"
#include "gui/config/ServerConfig.h"

class SettingsDialogButtonBox;

namespace Ui {
class SettingsDialog;
}

class SettingsDialog : public QDialog
{
  Q_OBJECT

public:
  void extracted();
  SettingsDialog(
      QWidget *parent, const ServerConfig &serverConfig,
      deskflow::gui::WindowsAutoStart autoStart = deskflow::gui::WindowsAutoStart()
  );
  ~SettingsDialog() override;

  /// 仅登录启动项变化时返回 false，主窗口据此避免重启正在共享键鼠的 core。
  bool coreSettingsChanged() const;

Q_SIGNALS:
  void requestRemoveAllSettings();

protected:
  void changeEvent(QEvent *e) override;

private:
  void initConnections();
  void regenCertificates();
  void browseCertificatePath();
  void browseLogPath();
  void setLogToFile(bool logToFile);
  void accept() override;
  bool isClientMode() const;
  void updateTlsControls();
  void updateTlsControlsEnabled();
  void resetAllSettings();
  void updateText();

  /// @brief Load all settings.
  void loadFromConfig();

  /// @brief Updates the key length value based on the loaded file.
  void updateKeyLengthOnFile(const QString &path);

  /// @brief Enables controls when they should be.
  void updateControls();

  /// @brief update if the log level warning is shown
  void logLevelChanged();

  /**
   * @brief isModified
   * @return true when any client settings in the gui do not match the stored settings values.
   */
  bool isModified() const;

  /// 比较原有配置项，登录启动注册不属于运行中 core 的配置。
  bool isConfigModified() const;

  /// 从实际注册表刷新复选框和注册提示；不写入，也不覆盖 Windows 的禁用决定。
  void loadAutoStart();

  /// 仅刷新提示文案，切换语言时不能重载并覆盖尚未保存的启动选项。
  void updateAutoStartText();

  /**
   * @brief isDefault
   * @return true if all client settings match the default values
   */
  bool isDefault() const;

  /**
   * @brief Set the gui values to the defalut values for all client settings
   */
  void resetToDefault();

  /**
   * @brief setButtonBoxEnabledButtons
   * Enable / Disable the button box buttons based on the state of the gui
   */
  void setButtonBoxEnabledButtons() const;

  bool m_interfaceSetOnLoad = false;
  std::unique_ptr<Ui::SettingsDialog> ui;
  const ServerConfig &m_serverConfig;
  SettingsDialogButtonBox *m_buttonBox = nullptr;
  deskflow::gui::WindowsAutoStart m_autoStart;
  deskflow::gui::WindowsAutoStart::State m_autoStartState;
  bool m_coreSettingsChanged = false;
  /// 重新生成证书属于待保存操作；取消或重置时不能提前改写磁盘证书。
  bool m_regenerateCertificate = false;
};
