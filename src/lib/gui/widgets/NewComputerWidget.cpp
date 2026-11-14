/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NewComputerWidget.h"
#include "ComputerSetupModel.h"

#include <QDrag>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>

NewComputerWidget::NewComputerWidget(QWidget *parent) : QLabel(parent)
{
  // do nothing
}

void NewComputerWidget::mousePressEvent(QMouseEvent *)
{
  // 此名称会随拖放数据写入服务端连接配置，并参与客户端名称匹配，不能作为界面文字翻译。
  // 中文翻译“未命名”的 UTF-8 字节会被 core 配置解析器拒绝，导致整个键鼠服务端无法启动；
  // 使用固定的 ASCII 占位名，用户仍需将其改为对端实际的计算机名称后才能正确连接。
  Computer newComputer(QStringLiteral("Unnamed"));

  QByteArray itemData;
  QDataStream dataStream(&itemData, QIODevice::WriteOnly);
  dataStream << -1 << -1 << newComputer;

  auto *pMimeData = new QMimeData;
  pMimeData->setData(ComputerSetupModel::mimeType(), itemData);

  auto *pDrag = new QDrag(this);
  pDrag->setMimeData(pMimeData);
  pDrag->setPixmap(pixmap());
  pDrag->setHotSpot(QPoint(width() / 2, height() / 2));
  pDrag->exec(Qt::CopyAction, Qt::CopyAction);
}
