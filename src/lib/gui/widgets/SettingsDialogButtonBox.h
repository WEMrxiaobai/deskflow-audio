/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QWidget>

class QLabel;
class QDialogButtonBox;

class SettingsDialogButtonBox : public QWidget
{
  Q_OBJECT
public:
  explicit SettingsDialogButtonBox(QWidget *parent = nullptr);
  void enableSave(bool enable) const;
  void enableReset(bool enable) const;
  void enableRestoreDefaults(bool enable) const;
  /// 配置文件只读时，允许拥有独立存储的设置继续保存；其他对话框默认保持原行为。
  void allowIndependentSave(bool allow);
Q_SIGNALS:
  void accepted();
  void rejected();
  void reset();
  void restoreDefault();

protected:
  void changeEvent(QEvent *e) override;

private:
  void updateText();
  void settingsWritableChanged(bool writable);
  QDialogButtonBox *m_buttonBox = nullptr;
  QLabel *m_lblText = nullptr;
  QLabel *m_lblIcon = nullptr;
  bool m_independentSave = false;
};
