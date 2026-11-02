// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/ipc/WindowsIpcPeer.h"
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>
#include <iostream>

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
  using namespace deskflow::gui::ipc::detail;
  QLocalServer server;
  const auto name = QStringLiteral("deskflow-peer-test-") + QUuid::createUuid().toString(QUuid::Id128);
  if (!server.listen(name)) return 1;
  QLocalSocket client;
  client.connectToServer(name);
  if (!client.waitForConnected(2000) || !server.waitForNewConnection(2000)) return 2;
  if (!verifyPipeServer(&client, false, app.applicationFilePath())) return 3;
  if (verifyPipeServer(&client, false, app.applicationDirPath() + QStringLiteral("/wrong-program.exe"))) return 4;
  if (verifyPipeServer(&client, true, app.applicationFilePath())) return 5;
  std::cout << "IPC server PID and executable verification: PASS\n";
#endif
  return 0;
}
