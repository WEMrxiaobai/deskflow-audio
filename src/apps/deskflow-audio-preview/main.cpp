/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Optional developer CLI; the GUI uses Session directly, without a console.
#include <windows.h>

#include <commdlg.h>

#include "Session.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
std::atomic<bool> stopping{false};
BOOL WINAPI onControl(DWORD event)
{
  if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT) {
    stopping = true;
    return TRUE;
  }
  return FALSE;
}
std::filesystem::path choose(bool save)
{
  std::array<wchar_t, 32768> name{};
  if (save)
    wcscpy_s(name.data(), name.size(), L"Deskflow试听邀请.dshaudio");
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.lpstrFilter = L"Deskflow试听邀请\0*.dshaudio\0\0";
  dialog.lpstrFile = name.data();
  dialog.nMaxFile = static_cast<DWORD>(name.size());
  dialog.lpstrDefExt = L"dshaudio";
  dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)))
    throw std::runtime_error("已取消选择邀请文件");
  return name.data();
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCtrlHandler(onControl, TRUE);
  try {
    if (argc < 2) {
      std::cout << "Deskflow 音频共享开发预览\n--receive [邀请文件] 接收并播放\n--send [邀请文件] 发送当前系统声音\n";
      return 0;
    }
    const std::wstring mode = argv[1];
    const bool testRender = mode == L"--test-render";
    const bool testCapture = mode == L"--test-capture";
    const bool receive = mode == L"--receive" || mode == L"--test-receive" || testRender;
    const bool test = mode == L"--test-receive" || mode == L"--test-send" || testRender || testCapture;
    if (testCapture)
      std::cout << "Explicit test: 5 seconds of system loopback capture to invited receiver; no recording.\n";
    if (testRender)
      std::cout << "Explicit test: 5 seconds of quiet audio through default output.\n";
    if ((!receive && mode != L"--send" && mode != L"--test-send" && !testCapture) || argc > 3)
      throw std::runtime_error("未知启动参数");
    if (!test && MessageBoxW(
                     nullptr,
                     receive ? L"接收预览会在局域网等待一个持有临时邀请的发送端，并在默认声卡播放。邀请含临时授权凭据，"
                               L"请只通过可信方式交给指定电脑；10分钟后失效。请调低耳机音量。\n\n继续？"
                             : L"发送预览会采集当前默认声卡的全部系统声音，并发给邀请指定的接收电脑。请先播放音乐。请勿"
                               L"同时运行接收预览，以免声音回传。\n\n继续？",
                     L"Deskflow 音频共享 · 开发预览", MB_OKCANCEL | MB_ICONWARNING
                 ) != IDOK)
      return 0;
    if (test && argc != 3)
      throw std::runtime_error("测试模式需指定临时邀请路径");
    const auto path = argc == 3 ? std::filesystem::path(argv[2]) : choose(receive);
    preview::runSession(
        {receive, path, test, testRender, testCapture}, stopping,
        [](const preview::SessionEvent &event) {
          auto &stream = event.state == preview::SessionState::Failed ? std::cerr : std::cout;
          stream << event.message << '\n' << std::flush;
        }
    );
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "预览停止：" << e.what() << '\n';
    return 1;
  }
}
