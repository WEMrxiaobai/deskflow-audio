// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#include <atomic>
#include <cwchar>
#include <list>
#include <string>

namespace deskflow::audio::windows::detail {
// One worker publishes immutable IDs; COM notification threads only read them.
// Keep published IDs alive until the notification sink's final COM release.
class DeviceNotificationFilter
{
public:
  explicit DeviceNotificationFilter(bool followsDefault) : m_followsDefault(followsDefault) {}
  void setEndpoint(const wchar_t *id)
  {
    for (const auto &existing : m_ids)
      if (existing == id) {
        m_endpoint.store(existing.c_str(), std::memory_order_release);
        return;
      }
    m_ids.emplace_back(id);
    m_endpoint.store(m_ids.back().c_str(), std::memory_order_release);
  }
  [[nodiscard]] bool endpointChanged(const wchar_t *id) const noexcept
  {
    const auto *endpoint = m_endpoint.load(std::memory_order_acquire);
    return id && endpoint && std::wcscmp(id, endpoint) == 0;
  }
  [[nodiscard]] bool defaultChanged(bool render, bool console) const noexcept
  {
    return m_followsDefault && render && console;
  }
private:
  const bool m_followsDefault;
  std::list<std::wstring> m_ids;
  std::atomic<const wchar_t *> m_endpoint{nullptr};
};
} // namespace deskflow::audio::windows::detail
