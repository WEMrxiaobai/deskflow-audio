/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string_view>

namespace deskflow {

/// GUI 和 core 共用连接名称规则，避免界面允许保存、core 却无法加载同一名称。
inline bool isValidComputerName(std::string_view name)
{
  // 名称用于协议身份和配置语法，按 ASCII 字节检查，不受系统语言或 char 符号位影响。
  if (name.empty() || name.size() > 255)
    return false;

  const auto isNameBoundary = [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
           value == '_';
  };
  for (std::size_t begin = 0; begin < name.size();) {
    auto end = name.find('.', begin);
    if (end == std::string_view::npos)
      end = name.size();

    // 域名各段不能为空，连字符仅能出现在段内部；下划线保持原有协议兼容性。
    if (end == begin || !isNameBoundary(name[begin]) || !isNameBoundary(name[end - 1]))
      return false;
    for (auto index = begin; index < end; ++index) {
      if (!isNameBoundary(name[index]) && name[index] != '-')
        return false;
    }

    // 保留 core 原先约定的末尾句点支持，例如旧 macOS 主机名 "host.local."。
    begin = end + 1;
  }
  return true;
}

} // namespace deskflow
