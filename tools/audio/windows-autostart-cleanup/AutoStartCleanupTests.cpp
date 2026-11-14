/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "AutoStartCleanup.h"

#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace deskflow::installer;
namespace {
const std::wstring kExecutable = L"C:\\中文 Program Files\\Deskflow\\deskflow.exe";
const wchar_t *kRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

/// 任意断言失败即结束该测试；测试失败也由 RAII 关闭句柄并移除专属测试子键。
void check(bool result, const char *message)
{
  if (!result)
    throw std::runtime_error(message);
}

struct Key
{
  HKEY value = nullptr;
  ~Key()
  {
    if (value)
      RegCloseKey(value);
  }
};

/// 创建测试专属键，不接触 HKU、ProfileList 或真实 Run。
void create(HKEY parent, const std::wstring &name, Key &key)
{
  check(
      RegCreateKeyExW(parent, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key.value, nullptr) ==
          ERROR_SUCCESS,
      "create isolated key"
  );
}

/// 测试用注册值始终写到调用方持有的隔离键。
void put(HKEY key, const wchar_t *name, const std::wstring &value)
{
  check(
      RegSetValueExW(
          key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))
      ) == ERROR_SUCCESS,
      "write isolated value"
  );
}

/// 读取存在性验证清理结果，不因忽略函数返回值而把未清理当成通过。
bool exists(HKEY key, const wchar_t *name)
{
  DWORD size = 0;
  return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

class Fixture
{
public:
  Fixture()
  {
    m_path = L"Software\\Deskflow\\Tests\\Uninstall-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++s_index);
    create(HKEY_CURRENT_USER, m_path, root);
  }
  ~Fixture()
  {
    // m_path 只由固定测试前缀和数字构造，递归清理的范围不会来自用户配置或真实注册表路径。
    RegDeleteTreeW(HKEY_CURRENT_USER, m_path.c_str());
  }
  Key root;

private:
  std::wstring m_path;
  inline static unsigned s_index = 0;
};

/// 同目录精确匹配、大小写和分隔符变化可清理；不同目录、参数、相对路径、畸形引号均保留。
void commandOwnership()
{
  check(ownsCommand(L"\"" + kExecutable + L"\"", kExecutable), "exact command");
  check(ownsCommand(L"\"c:/中文 Program Files/Deskflow/deskflow.EXE\"", kExecutable), "case and separators");
  check(ownsCommand(L"\"C:\\中文 Program Files\\Deskflow\\.\\deskflow.exe\"", kExecutable), "dot path");
  for (const auto &command :
       {L"\"C:\\portable\\deskflow.exe\"", L"\"C:\\other\\deskflow.exe\" --start", L"deskflow.exe", L"\"deskflow.exe\"",
        L"\"C:\\other\\deskflow.exe\"\""})
    check(!ownsCommand(command, kExecutable), "unrelated or ambiguous command preserved");
}

/// 真实 Win32 注册表删除必须幂等，并保留其他程序与另一个安装实例。
void deleteOwnedEntry()
{
  Fixture fixture;
  Key run;
  create(fixture.root.value, kRun, run);
  put(run.value, L"Deskflow", L"\"" + kExecutable + L"\"");
  put(run.value, L"OtherApplication", L"preserve");
  check(cleanRunKey(fixture.root.value, kExecutable, 0) == ERROR_SUCCESS, "clean owned command");
  check(!exists(run.value, L"Deskflow"), "owned command removed");
  check(exists(run.value, L"OtherApplication"), "other value preserved");
  check(cleanRunKey(fixture.root.value, kExecutable, 0) == ERROR_SUCCESS, "repeat cleanup");
  put(run.value, L"Deskflow", L"\"C:\\portable\\deskflow.exe\"");
  check(cleanRunKey(fixture.root.value, kExecutable, 0) == ERROR_SUCCESS, "other installation");
  check(exists(run.value, L"Deskflow"), "other installation preserved");
}

/// 非字符串或含额外参数的值安全保留，不将无法确认归属的内容删除。
void malformedValues()
{
  Fixture fixture;
  Key run;
  create(fixture.root.value, kRun, run);
  const DWORD value = 123;
  check(
      RegSetValueExW(run.value, L"Deskflow", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) ==
          ERROR_SUCCESS,
      "set non-string"
  );
  check(cleanRunKey(fixture.root.value, kExecutable, 0) == ERROR_SUCCESS, "ignore non-string");
  check(exists(run.value, L"Deskflow"), "non-string preserved");
  put(run.value, L"Deskflow", L"\"" + kExecutable + L"\" --other");
  check(cleanRunKey(fixture.root.value, kExecutable, 0) == ERROR_SUCCESS, "ignore additional arguments");
  check(exists(run.value, L"Deskflow"), "argument command preserved");
}

/// 同一批中覆盖已登录用户、未登录用户、损坏 hive 和另一个安装目录；失败不能中止后续用户。
void multipleUsers()
{
  Fixture fixture;
  Key users, profiles, loaded, offline, portable, bad, skipped, loadedRun, offlineRun, portableRun, skippedRun;
  create(fixture.root.value, L"Users", users);
  create(fixture.root.value, L"Profiles", profiles);
  create(users.value, L"S-1-5-21-1", loaded);
  create(fixture.root.value, L"OfflineUser", offline);
  create(users.value, L"S-1-12-1-3", portable);
  create(users.value, L"S-1-5-21-4.bak", skipped);
  create(loaded.value, kRun, loadedRun);
  create(offline.value, kRun, offlineRun);
  create(portable.value, kRun, portableRun);
  create(skipped.value, kRun, skippedRun);
  for (const auto key : {loadedRun.value, offlineRun.value, skippedRun.value})
    put(key, L"Deskflow", L"\"" + kExecutable + L"\"");
  put(portableRun.value, L"Deskflow", L"\"C:\\portable\\deskflow.exe\"");
  for (const auto sid : {L"S-1-5-21-0", L"S-1-5-21-1", L"S-1-5-21-2", L"S-1-12-1-3", L"S-1-5-21-4.bak"}) {
    Key profile;
    create(profiles.value, sid, profile);
    put(profile.value, L"ProfileImagePath", std::wstring(L"C:\\Users\\") + sid);
  }
  unsigned loads = 0;
  std::vector<LSTATUS> errors;
  cleanProfiles(
      users.value, profiles.value, kExecutable,
      [&](const std::wstring &path, HKEY *user) -> LSTATUS {
        ++loads;
        if (path != L"C:\\Users\\S-1-5-21-2\\NTUSER.DAT")
          return ERROR_ACCESS_DENIED;
        // 模拟私有 hive 加载，仅返回隔离测试子键，绝不加载任何真实 NTUSER.DAT。
        return RegOpenKeyExW(fixture.root.value, L"OfflineUser", 0, KEY_READ | KEY_WRITE, user);
      },
      [&](const std::wstring &, LSTATUS error) { errors.push_back(error); }
  );
  check(loads == 2, "only offline profiles loaded");
  check(errors.size() == 1, "one inaccessible profile logged");
  check(!exists(loadedRun.value, L"Deskflow"), "loaded user cleaned");
  check(!exists(offlineRun.value, L"Deskflow"), "offline user cleaned");
  check(exists(portableRun.value, L"Deskflow"), "portable instance preserved");
  check(exists(skippedRun.value, L"Deskflow"), "backup profile preserved");
}

/// 私有加载失败但用户同时完成登录时，应重查已加载用户并完成清理。
void loginRace()
{
  Fixture fixture;
  Key users, profiles, profile, user, run;
  create(fixture.root.value, L"Users", users);
  create(fixture.root.value, L"Profiles", profiles);
  create(profiles.value, L"S-1-5-21-1", profile);
  put(profile.value, L"ProfileImagePath", L"C:\\Users\\test");
  unsigned errors = 0;
  cleanProfiles(
      users.value, profiles.value, kExecutable,
      [&](const std::wstring &, HKEY *) -> LSTATUS {
        create(users.value, L"S-1-5-21-1", user);
        create(user.value, kRun, run);
        put(run.value, L"Deskflow", L"\"" + kExecutable + L"\"");
        return ERROR_SHARING_VIOLATION;
      },
      [&](const std::wstring &, LSTATUS) { ++errors; }
  );
  check(errors == 0 && !exists(run.value, L"Deskflow"), "concurrent login recovered");
}
} // namespace

/// 只运行隔离测试入口；本程序既不调用 MSI 清理导出，也不打开机器的真实用户根键。
int main()
{
  const std::vector<std::pair<const char *, void (*)()>> tests = {
      {"command ownership", commandOwnership},
      {"delete owned entry", deleteOwnedEntry},
      {"malformed values", malformedValues},
      {"multiple users", multipleUsers},
      {"login race", loginRace}
  };
  unsigned failed = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
  }
  return failed ? 1 : 0;
}
