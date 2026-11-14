/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "AutoStartCleanup.h"

#include <algorithm>
#include <vector>

namespace deskflow::installer {
namespace {
/// 无论正常返回或字符串分配/日志回调抛错，用户和 Run 句柄均须关闭，以便卸载临时 hive。
struct RegistryKey
{
  HKEY value = nullptr;
  ~RegistryKey()
  {
    if (value)
      RegCloseKey(value);
  }
};

/// 只接受绝对路径并作词法规范化；卸载时 exe 可能已删除，不能依赖文件存在或打开文件。
std::wstring normalizePath(std::wstring path)
{
  std::replace(path.begin(), path.end(), L'/', L'\\');
  const bool drive = path.size() > 2 && path[1] == L':' && path[2] == L'\\';
  const bool unc = path.size() > 2 && path[0] == L'\\' && path[1] == L'\\';
  if ((!drive && !unc) || path.find_first_of(L"\"\r\n") != std::wstring::npos)
    return {};
  const auto size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!size)
    return {};
  std::vector<wchar_t> buffer(size);
  const auto length = GetFullPathNameW(path.c_str(), size, buffer.data(), nullptr);
  return length && length < size ? std::wstring(buffer.data(), length) : std::wstring{};
}

/// 有界读取字符串，不把二进制、未终止字符串或环境变量命令误认作本项目启动项。
LSTATUS readString(HKEY key, const wchar_t *name, std::wstring &value, bool expand)
{
  DWORD type = 0;
  DWORD size = 0;
  auto status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
  if (status != ERROR_SUCCESS)
    return status;
  if ((type != REG_SZ && !(expand && type == REG_EXPAND_SZ)) || size < sizeof(wchar_t) || size > 65536 ||
      size % sizeof(wchar_t) != 0)
    return ERROR_INVALID_DATA;
  std::vector<wchar_t> buffer(size / sizeof(wchar_t));
  status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(buffer.data()), &size);
  if (status != ERROR_SUCCESS)
    return status;
  if (type != REG_SZ && !(expand && type == REG_EXPAND_SZ))
    return ERROR_INVALID_DATA;
  if (buffer.back() != L'\0')
    return ERROR_INVALID_DATA;
  value.assign(buffer.data());
  if ((value.size() + 1) * sizeof(wchar_t) != size)
    return ERROR_INVALID_DATA;
  if (type == REG_EXPAND_SZ && expand) {
    const auto needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!needed || needed > 32768)
      return ERROR_INVALID_DATA;
    std::vector<wchar_t> expanded(needed);
    if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed) != needed)
      return ERROR_INVALID_DATA;
    value.assign(expanded.data());
  }
  return ERROR_SUCCESS;
}
} // namespace

bool ownsCommand(const std::wstring &command, const std::wstring &executable)
{
  // 只处理精确的带引号路径。禁止从任意包含 deskflow.exe 的命令中截取并删除，避免影响其他实例。
  if (command.size() < 3 || command.front() != L'"' || command.back() != L'"')
    return false;
  const auto actual = normalizePath(command.substr(1, command.size() - 2));
  const auto expected = normalizePath(executable);
  return !actual.empty() && !expected.empty() &&
         CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

LSTATUS cleanRunKey(HKEY user, const std::wstring &executable, REGSAM view)
{
  RegistryKey run;
  auto status = RegOpenKeyExW(
      user, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE | view, &run.value
  );
  if (status == ERROR_FILE_NOT_FOUND)
    return ERROR_SUCCESS;
  if (status != ERROR_SUCCESS)
    return status;
  std::wstring command;
  status = readString(run.value, L"Deskflow", command, false);
  if (status == ERROR_SUCCESS && ownsCommand(command, executable))
    status = RegDeleteValueW(run.value, L"Deskflow");
  // 不认识的注册值安全保留；不存在是幂等成功，权限/IO 错误交给安装日志记录。
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_INVALID_DATA ? ERROR_SUCCESS : status;
}

void cleanProfiles(HKEY users, HKEY profiles, const std::wstring &executable, const HiveLoader &load, const Logger &log)
{
  for (DWORD index = 0;; ++index) {
    wchar_t name[256] = {};
    DWORD length = 256;
    auto status = RegEnumKeyExW(profiles, index, name, &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS)
      break;
    if (status != ERROR_SUCCESS) {
      log(L"enumerate user profiles", status);
      break;
    }
    const std::wstring sid(name, length);
    // 排除 .bak、服务账户和默认模板；仅覆盖本机/域用户与 Azure AD 用户的正式配置。
    if ((sid.rfind(L"S-1-5-21-", 0) != 0 && sid.rfind(L"S-1-12-1-", 0) != 0) ||
        sid.find_first_not_of(L"S-0123456789") != std::wstring::npos)
      continue;
    RegistryKey user;
    status = RegOpenKeyExW(users, sid.c_str(), 0, KEY_READ, &user.value);
    if (status == ERROR_FILE_NOT_FOUND) {
      RegistryKey profile;
      status = RegOpenKeyExW(profiles, sid.c_str(), 0, KEY_READ, &profile.value);
      std::wstring path;
      if (status == ERROR_SUCCESS) {
        status = readString(profile.value, L"ProfileImagePath", path, true);
      }
      if (status == ERROR_SUCCESS) {
        path = normalizePath(path);
        status = path.empty() ? ERROR_INVALID_DATA : load(path + L"\\NTUSER.DAT", &user.value);
        if (status != ERROR_SUCCESS) {
          // 用户恰在此时登录时临时加载会失败；重查 HKU，避免正常登录竞态留下启动项。
          const auto offlineError = status;
          status = RegOpenKeyExW(users, sid.c_str(), 0, KEY_READ, &user.value);
          if (status == ERROR_FILE_NOT_FOUND)
            status = offlineError;
        }
      }
    }
    if (status != ERROR_SUCCESS) {
      log(sid, status);
      continue;
    }
    // 两种注册视图分别核对目标。关闭用户句柄后，临时挂载的生命周期交给加载器统一清理。
    for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
      status = cleanRunKey(user.value, executable, view);
      if (status != ERROR_SUCCESS)
        log(sid, status);
    }
  }
}
} // namespace deskflow::installer
