/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/ComputerName.h"
#include "common/Settings.h"
#include "gui/config/ServerConfig.h"
#include "gui/core/CoreProcess.h"
#include "gui/dialogs/ComputerSettingsDialog.h"
#include "gui/dialogs/ServerConfigDialog.h"
#include "gui/validators/ComputerNameValidator.h"
#include "gui/widgets/NewComputerWidget.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDrag>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>

class ServerConfigurationValidationTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void nameRules_data();
  void nameRules();
  void defaultNameIgnoresTranslation();
  void savedChineseNameIsRejected();
  void duplicateNamesAndAliasesAreRejected();
  void invalidExportPreservesExistingFile();
  void invalidLayoutNeverStartsCore_data();
  void invalidLayoutNeverStartsCore();
  void unchangedInvalidNameCannotBeAccepted();
  void invalidLayoutCannotBeCommitted();
  void correctedLayoutCanBeSaved();

private:
  void loadLayout(const QString &clientName, const QStringList &aliases = {});
  QTemporaryDir m_directory;
  int m_nextFixture = 0;
};

void ServerConfigurationValidationTests::loadLayout(const QString &clientName, const QStringList &aliases)
{
  // 每个场景使用独立完整配置，复现“启动应用直接读到旧布局”，而不是只测试临时内存对象。
  const auto fileName = m_directory.filePath(QStringLiteral("layout-%1.conf").arg(++m_nextFixture));
  QSettings fixture(fileName, QSettings::IniFormat);
  fixture.setValue("core/computerName", "wsw");
  fixture.setValue("core/coreMode", static_cast<int>(Settings::CoreMode::Server));
  fixture.setValue("server/externalConfig", false);
  fixture.setValue("internalConfig/screens/size", 15);
  fixture.setValue("internalConfig/screens/8/name", "wsw");
  fixture.setValue("internalConfig/screens/3/name", clientName);
  fixture.setValue("computer_wsw/name", "wsw");
  fixture.setValue(QStringLiteral("computer_%1/name").arg(clientName), clientName);
  fixture.setValue(QStringLiteral("computer_%1/aliases").arg(clientName), aliases);
  fixture.sync();
  Settings::setStateFile(m_directory.filePath("state.conf"));
  Settings::setSettingsFile(fileName);
}

void ServerConfigurationValidationTests::nameRules_data()
{
  // 同时覆盖真实设备名、旧 macOS 末尾句点、长度边界和会破坏配置语法的输入。
  QTest::addColumn<QString>("name");
  QTest::addColumn<bool>("valid");
  QTest::newRow("server") << QStringLiteral("wsw") << true;
  QTest::newRow("client") << QStringLiteral("win10X") << true;
  QTest::newRow("underscore") << QStringLiteral("_work_pc_") << true;
  QTest::newRow("domain") << QStringLiteral("work-pc.local") << true;
  QTest::newRow("legacy-trailing-dot") << QStringLiteral("work-pc.local.") << true;
  QTest::newRow("length-255") << QString(255, QLatin1Char('a')) << true;
  QTest::newRow("length-256") << QString(256, QLatin1Char('a')) << false;
  QTest::newRow("empty") << QString() << false;
  QTest::newRow("translated-placeholder") << QStringLiteral("未命名") << false;
  QTest::newRow("unicode-letter") << QString::fromUtf8("café") << false;
  QTest::newRow("colon") << QStringLiteral("pc:client") << false;
  QTest::newRow("comment") << QStringLiteral("pc#client") << false;
  QTest::newRow("space") << QStringLiteral("work pc") << false;
  QTest::newRow("newline") << QStringLiteral("pc\nclient") << false;
  QTest::newRow("leading-dot") << QStringLiteral(".pc") << false;
  QTest::newRow("empty-domain-part") << QStringLiteral("pc..local") << false;
  QTest::newRow("leading-hyphen") << QStringLiteral("-pc") << false;
  QTest::newRow("trailing-hyphen") << QStringLiteral("pc-") << false;
}

void ServerConfigurationValidationTests::nameRules()
{
  QFETCH(QString, name);
  QFETCH(bool, valid);
  QCOMPARE(deskflow::isValidComputerName(name.toStdString()), valid);
  // 别名输入框允许暂时留空，其余输入必须与 core 的公共名称规则完全一致。
  validators::ComputerNameValidator validator("invalid name");
  QCOMPARE(validator.validate(name), name.isEmpty() || valid);
}

void ServerConfigurationValidationTests::savedChineseNameIsRejected()
{
  loadLayout(QStringLiteral("未命名"));
  ServerConfig config;
  QVERIFY(config.validationError().contains(QStringLiteral("未命名")));
}

void ServerConfigurationValidationTests::defaultNameIgnoresTranslation()
{
  // 安装真实翻译替身并走拖放入口，防止以后再次把 tr("Unnamed") 写回协议身份字段。
  class PlaceholderTranslator : public QTranslator
  {
  public:
    QString translate(const char *context, const char *source, const char *, int) const override
    {
      if (QByteArray(context) == "NewComputerWidget" && QByteArray(source) == "Unnamed")
        return QStringLiteral("未命名");
      return {};
    }
    bool isEmpty() const override
    {
      return false;
    }
  } translator;
  QApplication::installTranslator(&translator);
  const auto removeTranslator = qScopeGuard([&] { QApplication::removeTranslator(&translator); });
  NewComputerWidget widget(nullptr);
  QTimer::singleShot(100, &widget, [] { QDrag::cancel(); });
  QTest::mousePress(&widget, Qt::LeftButton);
  const auto drags = widget.findChildren<QDrag *>();
  QCOMPARE(drags.size(), qsizetype(1));
  QByteArray bytes = drags.front()->mimeData()->data("application/x-deskflow-computer");
  QDataStream stream(&bytes, QIODevice::ReadOnly);
  int column, row;
  Computer computer;
  stream >> column >> row >> computer;
  QCOMPARE(computer.name(), QStringLiteral("Unnamed"));
  QVERIFY(deskflow::isValidComputerName(computer.name().toStdString()));
}

void ServerConfigurationValidationTests::duplicateNamesAndAliasesAreRejected()
{
  // core 不区分大小写，布局中的电脑与别名相互冲突也必须在保存前拦截。
  loadLayout("WSW");
  QVERIFY(!ServerConfig().validationError().isEmpty());
  loadLayout("win10X", {"WSW"});
  QVERIFY(!ServerConfig().validationError().isEmpty());
  loadLayout("win10X", {"other", "OTHER"});
  QVERIFY(!ServerConfig().validationError().isEmpty());
  loadLayout("win10X", {QStringLiteral("未命名")});
  QVERIFY(ServerConfig().validationError().contains(QStringLiteral("未命名")));
}

void ServerConfigurationValidationTests::invalidExportPreservesExistingFile()
{
  loadLayout(QStringLiteral("未命名"));
  const auto path = m_directory.filePath("last-good.conf");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write("last-good-layout"), qint64(16));
  file.close();
  // 验证真实写入入口：校验失败不能先截断已有配置文件。
  QVERIFY(!ServerConfig().save(path));
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(file.readAll(), QByteArray("last-good-layout"));
}

void ServerConfigurationValidationTests::invalidLayoutNeverStartsCore_data()
{
  QTest::addColumn<Settings::ProcessMode>("mode");
  QTest::newRow("desktop") << Settings::ProcessMode::Desktop;
  QTest::newRow("service") << Settings::ProcessMode::Service;
}

void ServerConfigurationValidationTests::invalidLayoutNeverStartsCore()
{
  QFETCH(Settings::ProcessMode, mode);
  loadLayout(QStringLiteral("未命名"));
  const ServerConfig config;
  deskflow::gui::CoreProcess core(config);
  core.setMode(Settings::CoreMode::Server);
  int errors = 0;
  QObject::connect(&core, &deskflow::gui::CoreProcess::error, &core, [&](auto error) {
    QCOMPARE(error, deskflow::gui::CoreProcess::Error::InvalidServerConfig);
    ++errors;
  });
  core.start(mode);
  QCOMPARE(errors, 1);
  QVERIFY(core.serverConfigError().contains(QStringLiteral("未命名")));
  QCOMPARE(core.processState(), deskflow::core::ProcessState::Stopped);
  QCOMPARE(core.connectionState(), deskflow::core::ConnectionState::Disconnected);
  // 服务模式不能向现场 daemon 发出启动命令；桌面模式不能创建子进程或排队重试。
  QVERIFY(core.findChildren<QProcess *>().isEmpty());
  for (const auto *socket : core.findChildren<QLocalSocket *>())
    QCOMPARE(socket->state(), QLocalSocket::UnconnectedState);
  QTest::qWait(1100);
  QCOMPARE(errors, 1);
  QVERIFY(core.findChildren<QProcess *>().isEmpty());
}

void ServerConfigurationValidationTests::unchangedInvalidNameCannotBeAccepted()
{
  loadLayout("win10X");
  Computer computer(QStringLiteral("未命名"));
  ComputerList computers(5);
  computers.append(computer);
  ComputerSettingsDialog dialog(nullptr, &computer, &computers);
  auto *buttons = dialog.findChild<QDialogButtonBox *>("buttonBox");
  QVERIFY(buttons);
  // 不编辑输入框直接保存，复现旧名称先 setText、后安装 validator 而错误标签为空的路径。
  buttons->button(QDialogButtonBox::Save)->click();
  QCOMPARE(dialog.result(), int(QDialog::Rejected));
  QCOMPARE(computer.name(), QStringLiteral("未命名"));
  auto *error = dialog.findChild<QLabel *>("lblNameError");
  QVERIFY(error && !error->text().isEmpty());
}

void ServerConfigurationValidationTests::invalidLayoutCannotBeCommitted()
{
  loadLayout("win10X");
  ServerConfig original;
  const auto before = original;
  ServerConfigDialog dialog(nullptr, original);
  QVERIFY(dialog.addClient(QStringLiteral("未命名")));
  auto *buttons = dialog.findChild<SettingsDialogButtonBox *>();
  QVERIFY(buttons);
  bool sawNameError = false;
  // 在测试进程内部关闭校验提示框，同时核对提示内容；不能以“取消保存”替代验证真实保存路径。
  QTimer::singleShot(0, &dialog, [&] {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *message = qobject_cast<QMessageBox *>(widget)) {
        sawNameError = message->text().contains(QStringLiteral("未命名"));
        message->accept();
      }
    }
  });
  buttons->accepted();
  QVERIFY(sawNameError);
  QCOMPARE(dialog.result(), int(QDialog::Rejected));
  QVERIFY(original == before);
  QVERIFY(!Settings::knownComputers().contains(QStringLiteral("未命名")));
}

void ServerConfigurationValidationTests::correctedLayoutCanBeSaved()
{
  loadLayout("win10X");
  ServerConfig config;
  QVERIFY(config.validationError().isEmpty());
  const auto path = m_directory.filePath("corrected.conf");
  QVERIFY(config.save(path));
  QFile file(path);
  QVERIFY(file.open(QIODevice::ReadOnly));
  const auto bytes = file.readAll();
  QVERIFY(bytes.contains("win10X:"));
  QVERIFY(bytes.contains("up = win10X"));
  QVERIFY(bytes.contains("down = wsw"));
}

int main(int argc, char **argv)
{
  QApplication application(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  // Settings 单例首次构造也会清理配置；在接触它之前准备隔离存储，禁止读取现场配置。
  QTemporaryDir initial;
  if (!initial.isValid())
    return 2;
#ifdef Q_OS_WIN
  const auto portablePath = Settings::portableSettingsFile();
  if (!QDir().mkpath(QFileInfo(portablePath).absolutePath()))
    return 2;
  QFile portable(portablePath);
  if (!portable.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return 2;
  portable.close();
  const auto cleanupPortable = qScopeGuard([&] { QFile::remove(portablePath); });
#else
  qputenv("XDG_CONFIG_HOME", initial.path().toUtf8());
#endif
  Settings::setStateFile(initial.filePath("state.conf"));
  Settings::setSettingsFile(initial.filePath("settings.conf"));
  ServerConfigurationValidationTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "ServerConfigurationValidationTests.moc"
