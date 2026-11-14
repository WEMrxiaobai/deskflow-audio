/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Settings.h"
#include "gui/TlsUtility.h"
#include "gui/WindowsAutoStart.h"
#include "gui/dialogs/SettingsDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <aclapi.h>
#include <windows.h>

using deskflow::gui::WindowsAutoStart;

class WindowsAutoStartTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void init();
  void cleanup();
  void registerAndRemove();
  void repairMovedExecutable();
  void invalidPathPreservesEntry();
  void longPathIsRejected();
  void cancelDoesNotRegister();
  void startupOnlyDoesNotChangeCoreSettings();
  void existingEntryIsLoaded();
  void movedEntryCanBeRepairedWithoutRestart();
  void windowsSettingsButtonUsesStartupPage();
  void languageChangePreservesPendingStartup();
  void restoreDefaultsWaitsForSave();
  void coreChangesStillRequireApply();
  void logLevelChangesStillRequireApply();
  void registrationFailureKeepsDialogOpen();
  void readOnlyConfigCanManageStartup_data();
  void readOnlyConfigCanManageStartup();
  void startupPermissionIsIndependent();
  void tlsAndStartupChangesRequireApply();
  void cancelTlsChangesHasNoSideEffects();
  void resetTlsChangesHasNoSideEffects();
  void revertingTlsSelectionKeepsStartupIndependent();
  void tlsBatchNotifiesAfterAllValuesAreSet();
  void regenerateCertificateWaitsForSave();

private:
  // 每个测试只在随机测试子键中操作；既不用真实 Run，也不启动 stub exe、core 或音频。
  QTemporaryDir m_files;
  QString m_registryPath;
  QString m_executable;
  QUrl m_openedUrl;
  QString m_settingsFile;
  QString m_readOnlyFile;

public:
  /// 截获系统设置链接，验证真实按钮连接，同时避免测试弹出机器的系统设置。
  Q_INVOKABLE void captureStartupUrl(const QUrl &url)
  {
    m_openedUrl = url;
  }
};

void WindowsAutoStartTests::init()
{
  QVERIFY(m_files.isValid());
  m_registryPath = QStringLiteral("HKEY_CURRENT_USER\\Software\\Deskflow\\Tests\\AutoStart-%1")
                       .arg(QUuid::createUuid().toString(QUuid::Id128));
  const auto directory = m_files.path() + QStringLiteral("/中文 Program Files");
  QVERIFY(QDir().mkpath(directory));
  m_executable = directory + QStringLiteral("/deskflow.exe");
  QFile file(m_executable);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.close();
  Settings::setValue(Settings::Core::Port, 24800);
  Settings::setValue(Settings::Gui::AutoStartCore, true);
  Settings::setValue(Settings::Security::KeySize, 2048);
  Settings::setValue(Settings::Security::Certificate, m_files.path() + QStringLiteral("/certificate.pem"));
  Settings::setValue(Settings::Security::TlsEnabled, true);
  QFile::remove(Settings::value(Settings::Security::Certificate).toString());
  m_settingsFile = Settings::settingsFile();
}

void WindowsAutoStartTests::cleanup()
{
  // 即使用例断言提前返回，也恢复隔离配置的文件属性和单例路径，避免污染后续测试。
  if (!m_readOnlyFile.isEmpty()) {
    QVERIFY(QFile::setPermissions(m_readOnlyFile, QFile::ReadOwner | QFile::WriteOwner));
    Settings::setSettingsFile(m_settingsFile);
    QVERIFY(QFile::remove(m_readOnlyFile));
    m_readOnlyFile.clear();
  }
  // 删除范围仅为本次 UUID 子键；其他启动项和安装实例的配置始终不参与测试。
  QVERIFY(m_registryPath.startsWith(QStringLiteral("HKEY_CURRENT_USER\\Software\\Deskflow\\Tests\\AutoStart-")));
  QSettings registry(m_registryPath, QSettings::NativeFormat);
  registry.remove(QString());
  registry.sync();
  QCOMPARE(registry.status(), QSettings::NoError);
}

void WindowsAutoStartTests::registerAndRemove()
{
  WindowsAutoStart startup(m_executable, m_registryPath);
  QVERIFY(!startup.state().registered);
  QSettings registry(m_registryPath, QSettings::NativeFormat);
  registry.setValue(QStringLiteral("OtherApplication"), QStringLiteral("leave-me-alone"));
  registry.sync();
  QVERIFY(startup.setEnabled(true).isEmpty());
  QVERIFY(startup.state().registered);
  QVERIFY(startup.state().currentExecutable);
  registry.sync();
  QCOMPARE(
      registry.value(QStringLiteral("Deskflow")).toString(),
      QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(m_executable))
  );
  // 重复保存必须幂等；移除只删除 Deskflow 值，保留其他应用条目。
  QVERIFY(startup.setEnabled(true).isEmpty());
  QVERIFY(startup.setEnabled(false).isEmpty());
  QVERIFY(startup.setEnabled(false).isEmpty());
  QVERIFY(!startup.state().registered);
  registry.sync();
  QCOMPARE(registry.value(QStringLiteral("OtherApplication")).toString(), QStringLiteral("leave-me-alone"));
}

void WindowsAutoStartTests::repairMovedExecutable()
{
  QSettings registry(m_registryPath, QSettings::NativeFormat);
  registry.setValue(QStringLiteral("Deskflow"), QStringLiteral("\"C:\\old place\\deskflow.exe\""));
  registry.sync();
  WindowsAutoStart startup(m_executable, m_registryPath);
  QVERIFY(startup.state().registered);
  QVERIFY(!startup.state().currentExecutable);
  QVERIFY(startup.setEnabled(true).isEmpty());
  QVERIFY(startup.state().currentExecutable);
}

void WindowsAutoStartTests::invalidPathPreservesEntry()
{
  WindowsAutoStart valid(m_executable, m_registryPath);
  QVERIFY(valid.setEnabled(true).isEmpty());
  for (const auto &path :
       {QStringLiteral("relative.exe"), m_files.path() + QStringLiteral("/missing.exe"), m_files.path()}) {
    QVERIFY(!WindowsAutoStart(path, m_registryPath).setEnabled(true).isEmpty());
    QVERIFY(valid.state().currentExecutable);
  }
  // 即使 exe 已被移走，用户仍必须能关闭遗留的启动项。
  QVERIFY(WindowsAutoStart(QStringLiteral("missing.exe"), m_registryPath).setEnabled(false).isEmpty());
  QVERIFY(!valid.state().registered);
}

void WindowsAutoStartTests::longPathIsRejected()
{
  // 使用真实存在的长路径，确保验证的是 Run 的命令长度限制，而不是文件不存在。
  auto directory = m_files.path();
  while (directory.size() < 265)
    directory += QStringLiteral("/long-directory");
  QVERIFY(QDir().mkpath(directory));
  const auto path = directory + QStringLiteral("/deskflow.exe");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.close();
  WindowsAutoStart startup(path, m_registryPath);
  QVERIFY(!startup.setEnabled(true).isEmpty());
  QVERIFY(!startup.state().registered);
}

void WindowsAutoStartTests::cancelDoesNotRegister()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  auto *checkbox = dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"));
  QVERIFY(checkbox);
  checkbox->setChecked(true);
  dialog.reject();
  QVERIFY(!WindowsAutoStart(m_executable, m_registryPath).state().registered);
  QVERIFY(!QFile::exists(Settings::value(Settings::Security::Certificate).toString()));
}

void WindowsAutoStartTests::startupOnlyDoesNotChangeCoreSettings()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  auto *checkbox = dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"));
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  QVERIFY(checkbox && buttons);
  QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  checkbox->setChecked(true);
  QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
  buttons->button(QDialogButtonBox::Save)->click();
  QCOMPARE(dialog.result(), QDialog::Accepted);
  QVERIFY(!dialog.coreSettingsChanged());
  QCOMPARE(changed.count(), 0);
  QVERIFY(Settings::value(Settings::Gui::AutoStartCore).toBool());
  QVERIFY(WindowsAutoStart(m_executable, m_registryPath).state().currentExecutable);
}

void WindowsAutoStartTests::existingEntryIsLoaded()
{
  WindowsAutoStart startup(m_executable, m_registryPath);
  QVERIFY(startup.setEnabled(true).isEmpty());
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, startup);
  auto *checkbox = dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"));
  auto *status = dialog.findChild<QLabel *>(QStringLiteral("lblStartupStatus"));
  QVERIFY(checkbox && status);
  QVERIFY(checkbox->isChecked());
  // 提示必须区分“注册成功”和“Windows 已允许执行”，避免对系统禁用状态作出错误承诺。
  QVERIFY(status->text().contains(QStringLiteral("Windows")));
  checkbox->setChecked(false);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QCOMPARE(dialog.result(), QDialog::Accepted);
  QVERIFY(!dialog.coreSettingsChanged());
  QVERIFY(!startup.state().registered);
}

void WindowsAutoStartTests::restoreDefaultsWaitsForSave()
{
  WindowsAutoStart startup(m_executable, m_registryPath);
  QVERIFY(startup.setEnabled(true).isEmpty());
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, startup);
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  QVERIFY(buttons);
  buttons->button(QDialogButtonBox::RestoreDefaults)->click();
  QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->isChecked());
  QVERIFY(startup.state().registered);
  // Reset 重新从系统读取；两个界面按钮都不能在确认保存之前删除启动项。
  buttons->button(QDialogButtonBox::Reset)->click();
  QVERIFY(dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->isChecked());
  dialog.reject();
  QVERIFY(startup.state().registered);
}

void WindowsAutoStartTests::movedEntryCanBeRepairedWithoutRestart()
{
  QSettings registry(m_registryPath, QSettings::NativeFormat);
  registry.setValue(QStringLiteral("Deskflow"), QStringLiteral("\"C:\\previous-folder\\deskflow.exe\""));
  registry.sync();
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  QVERIFY(dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->isChecked());
  QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
  buttons->button(QDialogButtonBox::Save)->click();
  QCOMPARE(dialog.result(), QDialog::Accepted);
  QVERIFY(!dialog.coreSettingsChanged());
  QVERIFY(WindowsAutoStart(m_executable, m_registryPath).state().currentExecutable);
}

void WindowsAutoStartTests::windowsSettingsButtonUsesStartupPage()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  m_openedUrl.clear();
  QDesktopServices::setUrlHandler(QStringLiteral("ms-settings"), this, "captureStartupUrl");
  dialog.findChild<QPushButton *>(QStringLiteral("btnWindowsStartup"))->click();
  QDesktopServices::unsetUrlHandler(QStringLiteral("ms-settings"));
  QCOMPARE(m_openedUrl, QUrl(QStringLiteral("ms-settings:startupapps")));
  QVERIFY(!WindowsAutoStart(m_executable, m_registryPath).state().registered);
}

void WindowsAutoStartTests::languageChangePreservesPendingStartup()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  auto *checkbox = dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"));
  checkbox->setChecked(true);
  // 模拟 Qt 的真实重翻译事件：动态提示必须重填，尚未保存的用户选择不能被注册表覆盖。
  QEvent languageChange(QEvent::LanguageChange);
  QApplication::sendEvent(&dialog, &languageChange);
  QVERIFY(checkbox->isChecked());
  QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("lblStartupStatus"))->text().isEmpty());
  QVERIFY(!WindowsAutoStart(m_executable, m_registryPath).state().registered);
  dialog.reject();
}

void WindowsAutoStartTests::coreChangesStillRequireApply()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  dialog.findChild<QSpinBox *>(QStringLiteral("sbPort"))->setValue(24850);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QCOMPARE(dialog.result(), QDialog::Accepted);
  QVERIFY(dialog.coreSettingsChanged());
  QCOMPARE(Settings::value(Settings::Core::Port).toInt(), 24850);
}

void WindowsAutoStartTests::logLevelChangesStillRequireApply()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  auto *log = dialog.findChild<QComboBox *>(QStringLiteral("comboLogLevel"));
  log->setCurrentIndex(log->findData(QStringLiteral("DEBUG")));
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QCOMPARE(dialog.result(), QDialog::Accepted);
  QVERIFY(dialog.coreSettingsChanged());
  QCOMPARE(Settings::value(Settings::Log::Level).toString(), QStringLiteral("DEBUG"));
}

void WindowsAutoStartTests::registrationFailureKeepsDialogOpen()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(QStringLiteral("missing.exe"), m_registryPath));
  dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->setChecked(true);
  dialog.findChild<QSpinBox *>(QStringLiteral("sbPort"))->setValue(24860);
  QSignalSpy accepted(&dialog, &QDialog::accepted);
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  bool sawError = false;
  // 自动关闭预期错误框，以真实运行保存失败路径；超时兜底避免回归套件挂起。
  QTimer timer;
  connect(&timer, &QTimer::timeout, &dialog, [&] {
    if (auto *error = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      sawError = !error->text().isEmpty();
      error->accept();
    }
  });
  timer.start(10);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QVERIFY(sawError);
  QCOMPARE(accepted.count(), 0);
  QCOMPARE(changed.count(), 0);
  QCOMPARE(Settings::value(Settings::Core::Port).toInt(), 24800);
  QVERIFY(!WindowsAutoStart(m_executable, m_registryPath).state().registered);
}

void WindowsAutoStartTests::readOnlyConfigCanManageStartup_data()
{
  QTest::addColumn<bool>("registered");
  QTest::addColumn<bool>("enabled");
  QTest::addColumn<bool>("oldPath");
  QTest::newRow("enable") << false << true << false;
  QTest::newRow("disable") << true << false << false;
  QTest::newRow("repair") << true << true << true;
}

void WindowsAutoStartTests::readOnlyConfigCanManageStartup()
{
  QFETCH(bool, registered);
  QFETCH(bool, enabled);
  QFETCH(bool, oldPath);
  WindowsAutoStart startup(m_executable, m_registryPath);
  if (registered) {
    QVERIFY(startup.setEnabled(true).isEmpty());
    if (oldPath) {
      QSettings registry(m_registryPath, QSettings::NativeFormat);
      registry.setValue(QStringLiteral("Deskflow"), QStringLiteral("\"C:\\old\\deskflow.exe\""));
      registry.sync();
    }
  }
  Settings::save();
  m_readOnlyFile = m_files.path() + QStringLiteral("/readonly.conf");
  QVERIFY(QFile::copy(m_settingsFile, m_readOnlyFile));
  QVERIFY(QFile::setPermissions(m_readOnlyFile, QFile::ReadOwner));
  Settings::setSettingsFile(m_readOnlyFile);
  QVERIFY(!Settings::isWritable());
  QFile file(m_readOnlyFile);
  QVERIFY(file.open(QIODevice::ReadOnly));
  const auto before = file.readAll();
  file.close();

  ServerConfig server;
  SettingsDialog dialog(nullptr, server, startup);
  dialog.show();
  auto *checkbox = dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"));
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  auto *port = dialog.findChild<QSpinBox *>(QStringLiteral("sbPort"));
  QVERIFY(checkbox->isEnabled());
  QVERIFY(!port->isEnabled());
  QVERIFY(buttons->button(QDialogButtonBox::Save)->isVisible());
  QVERIFY(!buttons->button(QDialogButtonBox::RestoreDefaults)->isVisible());
  checkbox->setChecked(enabled);
  // 即使禁用控件被代码改动，启动项单独保存也不得写只读配置或要求重启 core。
  port->setValue(24999);
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  QSignalSpy accepted(&dialog, &QDialog::accepted);
  QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
  buttons->button(QDialogButtonBox::Save)->click();
  QCOMPARE(accepted.count(), 1);
  QCOMPARE(changed.count(), 0);
  QVERIFY(!dialog.coreSettingsChanged());
  QCOMPARE(startup.state().registered, enabled);
  QCOMPARE(startup.state().currentExecutable, enabled);
  QCOMPARE(Settings::value(Settings::Core::Port).toInt(), 24800);
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(file.readAll(), before);
}

void WindowsAutoStartTests::startupPermissionIsIndependent()
{
  // 用例只修改自己的随机注册表键 ACL。保留 WRITE_DAC，确保失败/提前返回时仍可恢复权限。
  HKEY key = nullptr;
  QCOMPARE(
      RegCreateKeyExW(
          HKEY_CURRENT_USER, qUtf16Printable(m_registryPath.mid(18)), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key,
          nullptr
      ),
      ERROR_SUCCESS
  );
  const auto oldCommand = QStringLiteral("\"C:\\old place\\deskflow.exe\"");
  QCOMPARE(
      RegSetValueExW(
          key, L"Deskflow", 0, REG_SZ, reinterpret_cast<const BYTE *>(oldCommand.utf16()),
          static_cast<DWORD>((oldCommand.size() + 1) * sizeof(wchar_t))
      ),
      ERROR_SUCCESS
  );
  PACL original = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  QCOMPARE(
      GetSecurityInfo(
          key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, &original, nullptr, &descriptor
      ),
      ERROR_SUCCESS
  );
  auto cleanup = qScopeGuard([&] {
    SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, original, nullptr);
    LocalFree(descriptor);
    RegCloseKey(key);
  });
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_WORLD_SID_AUTHORITY;
  PSID everyone = nullptr;
  QVERIFY(AllocateAndInitializeSid(&authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone));
  auto freeSid = qScopeGuard([&] { FreeSid(everyone); });
  EXPLICIT_ACCESSW access = {};
  access.grfAccessPermissions = KEY_SET_VALUE | KEY_CREATE_SUB_KEY;
  access.grfAccessMode = DENY_ACCESS;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.ptstrName = static_cast<LPWSTR>(everyone);
  PACL denied = nullptr;
  QCOMPARE(SetEntriesInAclW(1, &access, original, &denied), ERROR_SUCCESS);
  auto freeAcl = qScopeGuard([&] { LocalFree(denied); });
  QCOMPARE(
      SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, denied, nullptr), ERROR_SUCCESS
  );
  WindowsAutoStart startup(m_executable, m_registryPath);
  QVERIFY(Settings::isWritable());
  QVERIFY(startup.state().error.isEmpty());
  QVERIFY(!startup.state().writable);
  QVERIFY(!startup.setEnabled(true).isEmpty());
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, startup);
  QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->isEnabled());
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
  dialog.findChild<QSpinBox *>(QStringLiteral("sbPort"))->setValue(24861);
  QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
  buttons->button(QDialogButtonBox::Save)->click();
  QVERIFY(dialog.coreSettingsChanged());
  QVERIFY(startup.state().registered);
  QVERIFY(!startup.state().currentExecutable);
}

void WindowsAutoStartTests::tlsAndStartupChangesRequireApply()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->setChecked(true);
  dialog.findChild<QComboBox *>(QStringLiteral("comboTlsKeyLength"))->setCurrentText(QStringLiteral("4096"));
  // 选择阶段不发 settingsChanged，主窗口不会提前重新生成证书。
  QCOMPARE(changed.count(), 0);
  QCOMPARE(Settings::value(Settings::Security::KeySize).toInt(), 2048);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QVERIFY(dialog.coreSettingsChanged());
  QCOMPARE(Settings::value(Settings::Security::KeySize).toInt(), 4096);
  QVERIFY(WindowsAutoStart(m_executable, m_registryPath).state().currentExecutable);
}

void WindowsAutoStartTests::cancelTlsChangesHasNoSideEffects()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->setChecked(true);
  dialog.findChild<QComboBox *>(QStringLiteral("comboTlsKeyLength"))->setCurrentText(QStringLiteral("4096"));
  // 真实点击生成按钮，取消后仍须没有磁盘证书或配置写入。
  dialog.findChild<QPushButton *>(QStringLiteral("btnTlsRegenCert"))->click();
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
  QVERIFY(!QFile::exists(Settings::value(Settings::Security::Certificate).toString()));
  QCOMPARE(changed.count(), 0);
  QVERIFY(!dialog.coreSettingsChanged());
  QCOMPARE(Settings::value(Settings::Security::KeySize).toInt(), 2048);
  QVERIFY(!WindowsAutoStart(m_executable, m_registryPath).state().registered);
}

void WindowsAutoStartTests::resetTlsChangesHasNoSideEffects()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  dialog.findChild<QComboBox *>(QStringLiteral("comboTlsKeyLength"))->setCurrentText(QStringLiteral("4096"));
  dialog.findChild<QPushButton *>(QStringLiteral("btnTlsRegenCert"))->click();
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  buttons->button(QDialogButtonBox::Reset)->click();
  QCOMPARE(changed.count(), 0);
  QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("comboTlsKeyLength"))->currentText(), QStringLiteral("2048"));
  QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
}

void WindowsAutoStartTests::revertingTlsSelectionKeepsStartupIndependent()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  auto *keySize = dialog.findChild<QComboBox *>(QStringLiteral("comboTlsKeyLength"));
  keySize->setCurrentText(QStringLiteral("4096"));
  keySize->setCurrentText(QStringLiteral("2048"));
  dialog.findChild<QCheckBox *>(QStringLiteral("cbStartAtLogin"))->setChecked(true);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QCOMPARE(changed.count(), 0);
  QVERIFY(!dialog.coreSettingsChanged());
}

void WindowsAutoStartTests::tlsBatchNotifiesAfterAllValuesAreSet()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  const auto certificate = m_files.path() + QStringLiteral("/selected-certificate.pem");
  const bool checkPeers = !Settings::value(Settings::Security::CheckPeers).toBool();
  dialog.findChild<QComboBox *>(QStringLiteral("comboTlsKeyLength"))->setCurrentText(QStringLiteral("4096"));
  dialog.findChild<QLineEdit *>(QStringLiteral("lineTlsCertPath"))->setText(certificate);
  dialog.findChild<QCheckBox *>(QStringLiteral("cbRequireClientCert"))->setChecked(checkPeers);
  dialog.findChild<QGroupBox *>(QStringLiteral("groupSecurity"))->setChecked(false);
  unsigned notifications = 0;
  // 模拟主窗口的订阅点：每次 TLS 通知都必须能读到完整最终配置，禁止中间状态触发证书生成。
  connect(Settings::instance(), &Settings::settingsChanged, &dialog, [&](const QString &key) {
    if (key != Settings::Security::Certificate && key != Settings::Security::KeySize &&
        key != Settings::Security::TlsEnabled && key != Settings::Security::CheckPeers)
      return;
    ++notifications;
    QCOMPARE(Settings::value(Settings::Security::KeySize).toInt(), 4096);
    QCOMPARE(Settings::value(Settings::Security::Certificate).toString(), certificate);
    QCOMPARE(Settings::value(Settings::Security::CheckPeers).toBool(), checkPeers);
    QVERIFY(!Settings::value(Settings::Security::TlsEnabled).toBool());
  });
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QCOMPARE(notifications, 1U);
  QVERIFY(dialog.coreSettingsChanged());
}

void WindowsAutoStartTests::regenerateCertificateWaitsForSave()
{
  ServerConfig server;
  SettingsDialog dialog(nullptr, server, WindowsAutoStart(m_executable, m_registryPath));
  const auto path = Settings::value(Settings::Security::Certificate).toString();
  dialog.findChild<QPushButton *>(QStringLiteral("btnTlsRegenCert"))->click();
  QVERIFY(!QFile::exists(path));
  QSignalSpy changed(Settings::instance(), &Settings::settingsChanged);
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QVERIFY(dialog.coreSettingsChanged());
  QVERIFY(deskflow::gui::TlsUtility::isCertValid(path));
  QCOMPARE(deskflow::gui::TlsUtility::getCertKeyLength(path), 2048);
  // 同路径同长度重新生成也要发出通知，确保主窗口指纹不会停留在旧证书。
  bool certificateNotified = false;
  for (const auto &arguments : changed)
    certificateNotified |= arguments.first().toString() == Settings::Security::Certificate;
  QVERIFY(certificateNotified);
  QVERIFY(QFile::remove(path));
}

int main(int argc, char **argv)
{
  // 在 Settings 单例构造前准备专属便携配置和 state 目录，禁止读写已安装应用的真实设置。
  QTemporaryDir state;
  if (!state.isValid()) {
    qCritical() << "Unable to create isolated state directory:" << state.errorString();
    return 2;
  }
  qputenv("XDG_STATE_HOME", state.path().toUtf8());
  QApplication app(argc, argv);
  const auto folder = QApplication::applicationDirPath() + QStringLiteral("/settings");
  if (!QDir().mkpath(folder))
    return 3;
  QLockFile lock(folder + QStringLiteral("/autostart-tests.lock"));
  if (!lock.tryLock(10000))
    return 4;
  const auto path = folder + QStringLiteral("/") + kAppName + QStringLiteral(".conf");
  QFile config(path);
  // CREATE_NEW：存在任何配置时立即失败，不能覆盖用户或其他测试的文件。
  if (!config.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return 5;
  // 验证工程不部署翻译文件，保持默认语言，避免强制不存在的语言造成原设置界面自身的脏状态。
  config.write("[core]\ncomputerName=autostart-tests\n[gui]\nenableUpdateCheck=false\n");
  config.close();
  WindowsAutoStartTests tests;
  // MSVC 下默认日志可能发往调试器；显式文本输出让 CTest 在失败时保留完整断言证据。
  auto arguments = app.arguments();
  if (!arguments.contains(QStringLiteral("-o")))
    arguments << QStringLiteral("-o") << QStringLiteral("-,txt");
  const auto result = QTest::qExec(&tests, arguments);
  Settings::save();
  if (!QFile::remove(path))
    return 6;
  return result;
}

#include "WindowsAutoStartTests.moc"
