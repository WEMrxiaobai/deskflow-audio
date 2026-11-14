/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Invitation.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace preview {
enum class SessionState
{
  Waiting,
  Connecting,
  Connected,
  LocalPlaybackOverridden,
  Recovering,
  WaitingForMedia,
  Recovered,
  Renewing,
  Diagnostic,
  Stopped,
  Failed
};
struct SessionEvent
{
  SessionState state;
  std::string message;
  std::uint64_t packets = 0;
  std::string reason; // 日志原因码独立于用户提示，不把协议实现细节放进状态标签。
};
// 消息仅经原 TLS 控制连接转发，并绑定 GUI 当前 peer/session 及独立 worker 邮箱。
// request 为本会话递增编号；response 携带发送端最近一次已加密媒体的位置。
struct RecoveryMessage
{
  bool response = false;
  std::uint64_t request = 0;
  std::uint16_t sequence = 0;
  std::uint32_t timestamp = 0;
  // false 明确表示本会话尚未产生首个加密媒体包，不能用虚构的零序号表示静默。
  // 放在末尾并默认 true，兼容既有携带位置的聚合初始化和恢复回复。
  bool hasMedia = true;
};
// GUI 与 Session worker 之间的单槽邮箱。每个角色只接收一种消息，重复请求合并；
// 不用于声卡或编码回调，容量固定，不会因网络故障积压无限消息。
class SessionRecovery final
{
public:
  void post(const RecoveryMessage &message)
  {
    std::lock_guard lock(m_mutex);
    m_pending = message;
  }
  std::optional<RecoveryMessage> take()
  {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_pending, std::nullopt);
  }

private:
  std::mutex m_mutex;
  std::optional<RecoveryMessage> m_pending;
};
// 纯合成集成测试向时钟驱动器报告真实媒体进度，不包含密钥、UDP 内容或设备引用。
// 已完成采集块只计已发送、已丢弃或已过期的块，驱动器不能靠推进时钟跳过尚未处理的工作。
struct SessionTestProgress
{
  std::uint64_t capturedBlocks = 0, completedBlocks = 0, sentPackets = 0, receivedPackets = 0;
};
struct SessionOptions
{
  bool receive = false;
  std::filesystem::path invitationPath;
  bool test = false;
  bool testRender = false;
  bool testCapture = false;
  // 仅合成/设备诊断模式使用：默认五秒，较长时序回归可覆盖声卡启动预算后的恢复超时。
  // 不影响正式共享的运行期限；测试仍须满足原媒体数量校验或主动停止。
  std::chrono::milliseconds testDuration{5000};
  // 仅限 test=true 且不访问声卡的正向集成测试；为空时始终使用真实单调时钟。
  // 回调在普通 Session worker 上执行，可用收发进度屏障推进单调媒体时间；握手、
  // 编码队列过期和外层测试看门狗仍用真实时间。禁止用于正式共享及真实声卡诊断。
  std::function<std::uint64_t(const SessionTestProgress &)> testClock;
  // 发送端可选本机播放策略；所有者必须保持原子变量存活，直到 runSession 和资源清理结束。
  // GUI 使用后台共享状态承载它，销毁控件不等待线程。空指针保留 CLI 的原行为。
  const std::atomic<bool> *localPlayback = nullptr;
  // Sender: offer delivered over the existing authenticated Deskflow TLS channel.
  // Mutually exclusive with invitationPath; no discovery or peer authorization is
  // performed here. Caller owns and erases this copy after runSession and cleanup.
  std::optional<Invitation> invitation;
  // Receiver: invoked once on the worker thread AFTER binding its random UDP port.
  // The reference is valid only during this call. Deliver/copy it only to the
  // intended already-trusted peer; never log/persist it. Queue delivery and return
  // promptly: the callback must not block or wait for UDP negotiation. Exceptions
  // fail closed. An empty invitationPath selects memory-only mode and requires it.
  std::function<void(const Invitation &)> onReady;
  // Applies to both roles, independently of the offer's credential expiry.
  std::chrono::milliseconds handshakeTimeout{10000};
  // 主程序集成时同时提供邮箱和可信发送回调；CLI 无控制连接时保持失败后重建语义。
  std::shared_ptr<SessionRecovery> recovery;
  std::function<void(const RecoveryMessage &)> onRecovery;
  // 固定媒体预缓冲实验值。运行时不调节码率、FEC 或声卡 PCM 水位。
  std::uint32_t prebufferMs = 20;
  // 媒体密钥使用单调时钟单独计时；到期后经现有重试创建新密钥/SSRC，禁止复用索引。
  std::chrono::milliseconds mediaKeyLifetime{std::chrono::hours(1)};
};
using SessionObserver = std::function<void(const SessionEvent &)>;
// Blocking control/worker-thread API: never call on the GUI thread. Callbacks run
// only on this thread, never from real-time audio callbacks. Caller authorizes
// capture/playback through its master switch and binds offers to an already-trusted
// peer using the original Deskflow pairing/TLS connection. This API never selects
// peers, authorizes identity, or displays dialogs. Legacy invitationPath remains
// available solely for explicit developer CLI/tests. Keep stop/options alive until
// this call and its cleanup finish (join or independently owned worker state).
// Errors emit Failed then throw; reporting
// observer exceptions are ignored, but onReady errors abort safe negotiation.
void runSession(const SessionOptions &, std::atomic<bool> &stop, const SessionObserver &observer = {});
} // namespace preview
