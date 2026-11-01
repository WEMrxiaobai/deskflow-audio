/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <windows.h>
#include <memory>
#include <string>

namespace deskflow::audio::windows {
// Parent-side Qt/COM-free RAII. All methods on one serialized control worker,
// never audio callbacks. The child owns the mute guard, watches a real parent
// process handle, and restores on parent exit or pipe EOF. No named IPC, shell,
// PID reopen, endpoint command-line text, or globally accessible stop event.
class MuteWatchdog final
{
public:
  MuteWatchdog() noexcept;
  ~MuteWatchdog();
  MuteWatchdog(const MuteWatchdog &) = delete;
  MuteWatchdog &operator=(const MuteWatchdog &) = delete;
  HRESULT start(const std::wstring &exactEndpointId) noexcept;
  HRESULT update(bool localPlayback) noexcept;
  HRESULT stop() noexcept;
  bool running() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Private fixed-size protocol shared only with the helper entry point. The only
// command-line arguments are the three explicitly inherited numeric handles.
namespace mute_watchdog_detail {
inline constexpr DWORD protocolVersion = 1;
inline constexpr DWORD maximumIdCharacters = 4096;
enum class Operation : DWORD { Initialize = 1, Mute, Restore, Quit };
struct Command { Operation operation; DWORD characters; };
struct Reply { DWORD version; HRESULT result; };
} // namespace mute_watchdog_detail
} // namespace deskflow::audio::windows
