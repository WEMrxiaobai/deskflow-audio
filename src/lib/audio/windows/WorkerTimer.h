/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <cstdint>
#include <windows.h>
namespace deskflow::audio::windows {
// Worker-only: never called on capture/render callbacks. Does not change the
// system-wide timer resolution. Old Windows falls back to the ordinary timer.
class WorkerTimer final
{
public:
  WorkerTimer() noexcept
  {
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    m_highResolution = m_timer != nullptr;
    if (!m_timer)
      m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }
  ~WorkerTimer()
  {
    if (m_timer)
      CloseHandle(m_timer);
  }
  WorkerTimer(const WorkerTimer &) = delete;
  WorkerTimer &operator=(const WorkerTimer &) = delete;
  [[nodiscard]] bool highResolution() const noexcept
  {
    return m_highResolution;
  }
  void waitMicroseconds(std::uint32_t microseconds) noexcept
  {
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(microseconds ? microseconds : 1) * 10;
    if (m_timer && SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
      (void)WaitForSingleObject(m_timer, INFINITE);
    } else
      Sleep(1);
  }

private:
  HANDLE m_timer = nullptr;
  bool m_highResolution = false;
};
} // namespace deskflow::audio::windows
