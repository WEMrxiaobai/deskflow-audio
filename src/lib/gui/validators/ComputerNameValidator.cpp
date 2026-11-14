/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2021 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerNameValidator.h"

#include "common/ComputerName.h"

namespace validators {

ComputerNameValidator::ComputerNameValidator(const QString &message) : IStringValidator(message)
{
  // do nothing
}

bool ComputerNameValidator::validate(const QString &input) const
{
  // 空值由名称输入框的 EmptyStringValidator 单独提示；别名输入框仍允许暂时留空。
  // 非空名称直接采用 core 相同的字节规则，不能让翻译文字或配置分隔符进入布局。
  return input.isEmpty() || deskflow::isValidComputerName(input.toStdString());
}

} // namespace validators
