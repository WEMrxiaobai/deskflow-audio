/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "AutoStartCleanup.h"

#include <cstdio>
#include <msi.h>
#include <msiquery.h>
#include <vector>

namespace {
/// 清理失败记录 MSI 日志，避免某个损坏/不可读用户 hive 阻止其他用户清理或阻塞卸载。
void logStatus(MSIHANDLE installer, const wchar_t *context, LSTATUS status)
{
  const auto record = MsiCreateRecord(0);
  if (!record)
    return;
  // 异常及析构路径同样调用日志，使用固定缓冲避免再次分配内存导致异常越过 MSI 边界。
  wchar_t message[1024] = {};
  _snwprintf_s(message, _TRUNCATE, L"Deskflow startup cleanup: %.512ls; status=%ld", context, status);
  MsiRecordSetStringW(record, 0, message);
  MsiProcessMessage(installer, INSTALLMESSAGE_INFO, record);
  MsiCloseHandle(record);
}

/// 枚举期间即使异常退出，也不能泄漏全机 ProfileList 的注册表句柄。
struct ProfileKey
{
  HKEY value = nullptr;
  ~ProfileKey()
  {
    if (value)
      RegCloseKey(value);
  }
};

/// 未登录用户的 NTUSER.DAT 包含不同 ACL，不能用 RegLoadAppKey；系统身份临时挂载到唯一 HKU 名称。
class OfflineHives
{
public:
  explicit OfflineHives(MSIHANDLE installer) : m_installer(installer)
  {
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &m_token))
      return;
    m_backupEnabled = enable(SE_BACKUP_NAME, m_backup);
    m_restoreEnabled = enable(SE_RESTORE_NAME, m_restore);
  }

  ~OfflineHives()
  {
    // cleanProfiles 已关闭用户和 Run 句柄；成功挂载的 hive 在成功、错误或异常路径均要卸载。
    for (const auto &mount : m_mounts) {
      const auto status = RegUnLoadKeyW(HKEY_USERS, mount.c_str());
      if (status != ERROR_SUCCESS)
        logStatus(m_installer, L"unload temporary user hive", status);
    }
    if (m_token) {
      if (m_backupEnabled)
        AdjustTokenPrivileges(m_token, FALSE, &m_backup, 0, nullptr, nullptr);
      if (m_restoreEnabled)
        AdjustTokenPrivileges(m_token, FALSE, &m_restore, 0, nullptr, nullptr);
      CloseHandle(m_token);
    }
  }

  /// 挂载名仅由固定前缀、进程号、时钟和计数构成；不会覆盖/卸载现有用户的 SID 根键。
  LSTATUS load(const std::wstring &path, HKEY *user)
  {
    if (!m_backupEnabled || !m_restoreEnabled)
      return ERROR_PRIVILEGE_NOT_HELD;
    const auto name = L"DeskflowAutoStartCleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(m_mounts.size());
    HKEY existing = nullptr;
    auto status = RegOpenKeyExW(HKEY_USERS, name.c_str(), 0, KEY_READ, &existing);
    if (status == ERROR_SUCCESS) {
      RegCloseKey(existing);
      return ERROR_ALREADY_EXISTS;
    }
    if (status != ERROR_FILE_NOT_FOUND)
      return status;
    // 先分配记录空间再挂载，避免分配失败导致成功挂载的 hive 无人负责卸载。
    m_mounts.push_back(name);
    status = RegLoadKeyW(HKEY_USERS, name.c_str(), path.c_str());
    if (status != ERROR_SUCCESS) {
      m_mounts.pop_back();
      return status;
    }
    return RegOpenKeyExW(HKEY_USERS, name.c_str(), 0, KEY_READ | KEY_WRITE, user);
  }

private:
  /// 仅为当前自定义操作临时启用权限，并保留旧状态供析构恢复；不修改用户 ACL 或系统策略。
  bool enable(const wchar_t *name, TOKEN_PRIVILEGES &previous)
  {
    TOKEN_PRIVILEGES requested = {};
    requested.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &requested.Privileges[0].Luid))
      return false;
    requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    DWORD size = sizeof(previous);
    SetLastError(ERROR_SUCCESS);
    return AdjustTokenPrivileges(m_token, FALSE, &requested, sizeof(previous), &previous, &size) &&
           GetLastError() == ERROR_SUCCESS;
  }

  MSIHANDLE m_installer;
  HANDLE m_token = nullptr;
  TOKEN_PRIVILEGES m_backup = {};
  TOKEN_PRIVILEGES m_restore = {};
  bool m_backupEnabled = false;
  bool m_restoreEnabled = false;
  std::vector<std::wstring> m_mounts;
};
} // namespace

/// WiX 以系统身份执行；目标路径只从计划阶段传入的 CustomActionData 读取，不依赖已删除的安装文件。
extern "C" __declspec(dllexport) UINT __stdcall CleanDeskflowAutoStart(MSIHANDLE installer)
{
  try {
    DWORD size = 0;
    wchar_t empty = L'\0';
    if (MsiGetPropertyW(installer, L"CustomActionData", &empty, &size) != ERROR_MORE_DATA || size > 32768)
      return ERROR_INSTALL_FAILURE;
    std::vector<wchar_t> data(size + 1);
    ++size;
    if (MsiGetPropertyW(installer, L"CustomActionData", data.data(), &size) != ERROR_SUCCESS)
      return ERROR_INSTALL_FAILURE;
    const std::wstring executable(data.data());
    if (!deskflow::installer::ownsCommand(L"\"" + executable + L"\"", executable))
      return ERROR_INSTALL_FAILURE;
    ProfileKey profiles;
    const auto status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0,
        KEY_READ | KEY_WOW64_64KEY, &profiles.value
    );
    if (status != ERROR_SUCCESS) {
      logStatus(installer, L"open ProfileList", status);
      return ERROR_INSTALL_FAILURE;
    }
    OfflineHives offline(installer);
    deskflow::installer::cleanProfiles(
        HKEY_USERS, profiles.value, executable,
        [&offline](const std::wstring &path, HKEY *user) { return offline.load(path, user); },
        [installer](const std::wstring &context, LSTATUS error) { logStatus(installer, context.c_str(), error); }
    );
    logStatus(installer, L"finished processing user profiles", ERROR_SUCCESS);
    return ERROR_SUCCESS;
  } catch (...) {
    logStatus(installer, L"unexpected cleanup failure", ERROR_INSTALL_FAILURE);
    return ERROR_INSTALL_FAILURE;
  }
}
