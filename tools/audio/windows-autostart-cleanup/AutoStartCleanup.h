/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace deskflow::installer {

// 句柄由调用方负责关闭；注入接口让测试仅访问随机测试子键，绝不枚举真实用户或挂载真实 NTUSER.DAT。
using HiveLoader = std::function<LSTATUS(const std::wstring &, HKEY *)>;
using Logger = std::function<void(const std::wstring &, LSTATUS)>;

/// 仅识别本项目写出的完整启动命令。歧义命令、额外参数和不同目录一律保留。
bool ownsCommand(const std::wstring &command, const std::wstring &executable);

/// 仅删除所属当前安装目录的 Deskflow 值；不存在和其他实例均不报错。
LSTATUS cleanRunKey(HKEY user, const std::wstring &executable, REGSAM view);

/// 按 ProfileList 中的用户 SID 逐个处理：优先已加载 HKU，未登录用户由加载器提供临时 hive。
void cleanProfiles(
    HKEY users, HKEY profiles, const std::wstring &executable, const HiveLoader &load, const Logger &log
);

} // namespace deskflow::installer
