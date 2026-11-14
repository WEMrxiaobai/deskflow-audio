/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

#include "Invitation.h"
#include "Session.h"
#include "audio/engine/AudioEngine.h"
#include "audio/windows/MuteWatchdog.h"
#include "audio/windows/WasapiBackend.h"
#include "audio/windows/WorkerTimer.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace ae = deskflow::audio::engine;
namespace aw = deskflow::audio::windows;
namespace at = deskflow::audio::transport;
using deskflow::audio::dsp::PcmFormat;
namespace {
// 设备打开与首包控制回复共用这一启动预算，避免接收端在发送端合法初始化期间重建会话。
constexpr std::uint64_t deviceStartupWaitNs = 5000000000ULL;
// 失败的诊断原因与用户可读提示分别传递，GUI 不需要解析或展示内部原因码。
class SessionFailure final : public std::runtime_error
{
public:
  SessionFailure(const char *reason, const char *message) : std::runtime_error(message), reason(reason)
  {
  }
  const char *reason;
};
void notify(
    const preview::SessionObserver &observer, preview::SessionState state, const std::string &message,
    std::uint64_t packets = 0, const char *reason = ""
) noexcept
{
  if (!observer)
    return;
  try {
    observer({state, message, packets, reason});
  } catch (...) {
  }
}
struct Network
{
  SOCKET socketHandle = INVALID_SOCKET;
  Network()
  {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
      throw std::runtime_error("WSAStartup failed");
    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
      WSACleanup();
      throw std::runtime_error("UDP socket failed");
    }
    u_long mode = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &mode)) {
      closesocket(socketHandle);
      WSACleanup();
      throw std::runtime_error("Nonblocking socket failed");
    }
  }
  ~Network()
  {
    closesocket(socketHandle);
    WSACleanup();
  }
  std::uint16_t bindReceiver()
  {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socketHandle, reinterpret_cast<sockaddr *>(&a), sizeof(a)))
      throw std::runtime_error("Cannot bind receiver");
    int size = sizeof(a);
    if (getsockname(socketHandle, reinterpret_cast<sockaddr *>(&a), &size))
      throw std::runtime_error("Cannot query receiver port");
    return ntohs(a.sin_port);
  }
  bool sendTo(std::span<const std::uint8_t> bytes, const sockaddr_in &a) noexcept
  {
    return sendto(
               socketHandle, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
               reinterpret_cast<const sockaddr *>(&a), sizeof(a)
           ) == static_cast<int>(bytes.size());
  }
  int receive(std::span<std::uint8_t> bytes, sockaddr_in &from)
  {
    int size = sizeof(from);
    const int n = recvfrom(
        socketHandle, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
        reinterpret_cast<sockaddr *>(&from), &size
    );
    if (n == SOCKET_ERROR) {
      const auto error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK || error == WSAEMSGSIZE || error == WSAECONNRESET)
        return 0;
      throw SessionFailure("udp-receive-error", "声音网络接收失败，正在重新连接");
    }
    return n;
  }
};
bool sameEndpoint(const sockaddr_in &a, const sockaddr_in &b)
{
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
std::vector<std::string> addresses()
{
  ULONG size = 16384;
  std::vector<std::uint8_t> memory(size);
  auto *head = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data());
  auto result = GetAdaptersAddresses(
      AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, head, &size
  );
  if (result == ERROR_BUFFER_OVERFLOW) {
    memory.resize(size);
    head = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data());
    result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, head, &size
    );
  }
  std::vector<std::string> out;
  if (result == NO_ERROR)
    for (auto *a = head; a && out.size() < 8; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        continue;
      for (auto *u = a->FirstUnicastAddress; u && out.size() < 8; u = u->Next) {
        if (u->Address.lpSockaddr->sa_family != AF_INET)
          continue;
        char text[INET_ADDRSTRLEN]{};
        const auto *ipv4 = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
        if (InetNtopA(AF_INET, const_cast<IN_ADDR *>(&ipv4->sin_addr), text, sizeof(text)))
          out.emplace_back(text);
      }
    }
  if (out.empty())
    out.emplace_back("127.0.0.1");
  return out;
}
struct Device
{
  std::atomic<ae::AudioSender *> sender{nullptr};
  std::atomic<ae::AudioReceiver *> receiver{nullptr};
  std::atomic<std::uint64_t> generation{0}, active{0}, renderedCallbacks{0}, capturedCallbacks{0};
  std::atomic<std::uint32_t> rate{0}, channels{0};
  std::atomic<bool> failed{false};
  static void status(void *p, const aw::Status &s) noexcept
  {
    auto &d = *static_cast<Device *>(p);
    if (s.state == aw::State::Running) {
      d.rate = s.format.sampleRate;
      d.channels = s.format.channels;
      d.generation.store(s.format.generation, std::memory_order_release);
    } else if (s.state == aw::State::Failed || s.state == aw::State::RetryPending)
      d.failed = true;
  }
  static void capture(void *p, std::span<const float> pcm, const aw::DeviceFormat &f, const aw::CaptureInfo &i) noexcept
  {
    auto &d = *static_cast<Device *>(p);
    if (f.generation != d.active.load())
      return;
    if (auto *s = d.sender.load()) {
      d.capturedCallbacks.fetch_add(1, std::memory_order_relaxed);
      (void)s->capture(pcm, i.discontinuity, ae::monotonicNowNs());
    }
  }
  static void render(void *p, std::span<float> pcm, const aw::DeviceFormat &f) noexcept
  {
    auto &d = *static_cast<Device *>(p);
    if (f.generation != d.active.load())
      return;
    if (auto *r = d.receiver.load()) {
      d.renderedCallbacks.fetch_add(1, std::memory_order_relaxed);
      (void)r->render(pcm);
    }
  }
};
struct InviteOwner
{
  preview::Invitation value;
  ~InviteOwner()
  {
    preview::erase(value);
  }
};
void runInternal(
    const preview::SessionOptions &options, std::atomic<bool> &stopping, const preview::SessionObserver &observer
)
{
  const auto receive = options.receive;
  const auto test = options.test;
  const auto testRender = options.testRender;
  const auto testCapture = options.testCapture;
  const auto &path = options.invitationPath;
  if (testRender && (!test || !receive))
    throw std::invalid_argument("testRender requires a receiving test session");
  if (testCapture && (!test || receive))
    throw std::invalid_argument("testCapture requires a sending test session");
  if (test &&
      (options.testDuration <= std::chrono::milliseconds::zero() || options.testDuration > std::chrono::minutes(1)))
    throw std::invalid_argument("testDuration must be between 1 and 60000 milliseconds");
  if (options.handshakeTimeout <= std::chrono::milliseconds::zero() ||
      options.handshakeTimeout > std::chrono::minutes(1))
    throw std::invalid_argument("handshakeTimeout must be between 1 and 60000 milliseconds");
  if (options.prebufferMs < 5 || options.prebufferMs > 80 ||
      options.mediaKeyLifetime <= std::chrono::milliseconds::zero() ||
      options.mediaKeyLifetime > std::chrono::hours(24))
    throw std::invalid_argument("Invalid media prebuffer or key lifetime");
  if (bool(options.recovery) != bool(options.onRecovery))
    throw std::invalid_argument("Media recovery requires both a trusted callback and mailbox");
  if (receive && options.invitation)
    throw std::invalid_argument("Receiver generates its own fresh invitation");
  if (!receive && options.invitation && !path.empty())
    throw std::invalid_argument("Specify either an authenticated invitation or a CLI invitation path, not both");
  if (receive && path.empty() && !options.onReady)
    throw std::invalid_argument("Memory-only receiver requires an onReady callback");
  if (!receive && !options.invitation && path.empty())
    throw std::invalid_argument("Sender requires an authenticated invitation or a CLI invitation path");
  if (!receive && options.onReady)
    throw std::invalid_argument("onReady is only available for receivers");
  if (stopping.load())
    return;
  HANDLE exclusive = nullptr;
  if (!test) {
    exclusive = CreateMutexW(nullptr, FALSE, L"Local\\DeskflowAudioPreviewActive");
    if (!exclusive || GetLastError() == ERROR_ALREADY_EXISTS) {
      if (exclusive)
        CloseHandle(exclusive);
      throw std::runtime_error("本Windows会话已经运行音频共享。请先停止另一会话，避免系统回环回传。");
    }
  }
  struct ExclusiveGuard
  {
    HANDLE handle;
    ~ExclusiveGuard()
    {
      if (handle)
        CloseHandle(handle);
    }
  } exclusiveGuard{exclusive};
  Network network; // Outlives encoder sink and engine teardown.
  InviteOwner invite;
  const auto handshakeStart = ae::monotonicNowNs();
  const auto deadline =
      handshakeStart +
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(options.handshakeTimeout).count()
      );
  if (receive) {
    invite.value =
        preview::createInvitation(network.bindReceiver(), test ? std::vector<std::string>{"127.0.0.1"} : addresses());
    if (!path.empty())
      preview::saveInvitation(path, invite.value);
    if (stopping.load())
      return;
    if (options.onReady)
      options.onReady(invite.value);
    notify(
        observer, preview::SessionState::Waiting,
        path.empty() ? "音频接收已就绪，正在通过已配对的可信连接协商。"
                     : "邀请已创建。请通过可信方式交给发送电脑；邀请10分钟失效，只允许一个发送端。"
    );
  } else
    invite.value = options.invitation ? *options.invitation : preview::loadInvitation(path);
  if (stopping.load())
    return;
  auto nonce = receive ? preview::Nonce{} : preview::randomNonce();
  sockaddr_in peer{};
  bool connected = false;
  std::array<std::uint8_t, 1201> packet{};
  aw::WorkerTimer timer;
  const auto hello =
      receive ? std::vector<std::uint8_t>{} : preview::controlPacket(invite.value, preview::ControlType::Hello, nonce);
  auto nextHello = ae::monotonicNowNs();
  notify(
      observer, receive ? preview::SessionState::Waiting : preview::SessionState::Connecting,
      receive ? "等待发送端授权连接..." : "正在连接接收电脑..."
  );
  while (!connected && !stopping && preview::unixSeconds() < invite.value.expiresUnixSeconds) {
    const auto now = ae::monotonicNowNs();
    if (now >= deadline) {
      if (stopping.load())
        return;
      throw SessionFailure(
          "udp-handshake-timeout",
          receive
              ? "等待可信发送端连接超时。请确认已配对电脑的音频共享已开启。"
              : "连接超时：确认同一局域网，接收端仍开启，且Windows允许此程序通过专用网络防火墙。不会自动修改防火墙。"
      );
    }
    if (!receive && now >= nextHello) {
      for (const auto &address : invite.value.addresses) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(invite.value.port);
        if (InetPtonA(AF_INET, address.c_str(), &target.sin_addr) == 1)
          (void)network.sendTo(hello, target);
      }
      nextHello = now + 500000000ULL;
    }
    sockaddr_in from{};
    const auto bytes = network.receive(packet, from);
    if (bytes) {
      preview::ControlType type{};
      preview::Nonce received{};
      if (preview::verifyControl(invite.value, std::span(packet.data(), bytes), type, received)) {
        if (receive && type == preview::ControlType::Hello) {
          peer = from;
          nonce = received;
          connected = true;
        }
        if (!receive && type == preview::ControlType::Ack && received == nonce) {
          bool expected = false;
          for (const auto &address : invite.value.addresses) {
            IN_ADDR ip{};
            if (InetPtonA(AF_INET, address.c_str(), &ip) == 1 && ip.s_addr == from.sin_addr.s_addr &&
                ntohs(from.sin_port) == invite.value.port)
              expected = true;
          }
          if (expected) {
            peer = from;
            connected = true;
          }
        }
      }
    }
    if (!connected)
      timer.waitMicroseconds(1000);
  }
  if (stopping.load())
    return;
  if (!connected)
    throw SessionFailure("handshake-credential-expired", "音频连接协商已过期，正在重新连接");
  // 冻结本次已认证握手的停止权限。后续邀请过期不结束运行会话，也不会屏蔽 STOP。
  preview::EstablishedSessionControl establishedControl(invite.value, nonce);
  // Prepared while throwing is still safe. Guard sends best-effort STOP after
  // backend/encoder destruction on normal exit AND exception unwinding.
  struct StopGuard
  {
    Network &network;
    sockaddr_in peer;
    std::vector<std::uint8_t> packet;
    ~StopGuard()
    {
      for (int i = 0; i < 3; ++i)
        (void)network.sendTo(packet, peer);
    }
  } stopGuard{network, peer, establishedControl.stopPacket()};
  auto session = preview::sessionMediaKey(invite.value, nonce);
  at::SrtpKey key(session);
  SecureZeroMemory(session.data(), session.size());
  at::StreamConfig config{invite.value.ssrc, 1, 48000, 2, 240};
  Device device;
  std::unique_ptr<ae::AudioSender> sender;
  std::unique_ptr<ae::AudioReceiver> receiver;
  aw::MuteWatchdog localOutput;               // Outlives backend; restores after callbacks stop on every exit.
  std::unique_ptr<aw::WasapiBackend> backend; // Stops callbacks before destroying engines.
  PcmFormat format{48000, 2};
  const bool useDevice = !test || testRender || testCapture;
  const bool manageLocalOutput = !receive && useDevice && options.localPlayback;
  std::wstring captureEndpoint;
  if (manageLocalOutput && FAILED(aw::WasapiBackend::defaultRenderEndpointId(captureEndpoint)))
    throw std::runtime_error("无法读取共享音频设备，本机静音未修改");
  if (useDevice) {
    aw::Callbacks callbacks{&device, Device::capture, Device::render, Device::status};
    backend = std::make_unique<aw::WasapiBackend>(
        receive ? aw::Direction::Render : aw::Direction::LoopbackCapture, callbacks, captureEndpoint
    );
    if (!backend->start())
      throw std::runtime_error("无法启动声卡");
    const auto until = ae::monotonicNowNs() + deviceStartupWaitNs;
    while (!stopping.load() && !device.generation && ae::monotonicNowNs() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stopping.load())
      return;
    if (!device.generation)
      throw std::runtime_error("5秒内未打开默认声卡");
    format = {device.rate.load(), device.channels.load()};
    device.active = device.generation.load();
  }
  if (receive) {
    ae::ReceiverOptions receiverOptions;
    receiverOptions.initialVolume = .35f;
    receiverOptions.prebufferMs = options.prebufferMs;
    receiver = std::make_unique<ae::AudioReceiver>(format, 5, config, std::move(key), receiverOptions);
    device.receiver = receiver.get();
  } else {
    sender = std::make_unique<ae::AudioSender>(format, 5, config, std::move(key), [&](auto bytes) {
      return network.sendTo(bytes, peer);
    });
    sender->start();
    device.sender = sender.get();
  }
  if (manageLocalOutput) {
    if (FAILED(localOutput.start(captureEndpoint)) || FAILED(localOutput.update(options.localPlayback->load())))
      throw std::runtime_error("无法安全控制本机播放，已停止共享并恢复本机声音");
  }
  // 接收端设备就绪后才发送 ACK，发送端随后才开始打开声卡；首包预算从这里计时，
  // 不能从接收 HELLO 时起算，否则接收端自己的设备初始化也会耗掉发送端的启动余量。
  const auto mediaReadyAt = ae::monotonicNowNs();
  if (receive)
    (void)network.sendTo(preview::controlPacket(invite.value, preview::ControlType::Ack, nonce), peer);
  notify(
      observer, preview::SessionState::Connected,
      receive ? (test && !testRender ? "已连接：测试模式仅解码丢弃，不播放。"
                                     : "已连接：正在播放远端系统声音（初始音量35%）。")
              : "已连接：正在发送系统声音，请在本机播放音乐。"
  );
  auto start = ae::monotonicNowNs(), nextTone = start, nextRender = start, nextReport = start;
  const auto keyDeadline =
      start +
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(options.mediaKeyLifetime).count()
      );
  // 恢复编号仅在此媒体会话内递增；TLS 外层还会检查 peer/session/generation。
  std::uint64_t requestSerial = 0, pendingRequest = 0, recoveryDeadline = 0, senderRequest = 0, answeredRequest = 0;
  bool recoveryAuthorized = false, keyExpired = false;
  std::uint64_t pendingRecoveryEpoch = 0;
  std::optional<std::uint64_t> authorizedEpoch;
  bool waitingForMedia = false;
  std::optional<ae::MediaPosition> lastSourcePosition;
  // 常规恢复的控制回复最多等待三秒；有发送进度证据后再给媒体一秒到达宽限。
  // 此硬期限不随新回复顺延，避免 UDP 持续不可达时被正常 TLS 心跳无限续命。
  constexpr std::uint64_t controlWaitNs = 3000000000ULL, mediaArrivalGraceNs = 1000000000ULL;
  // 首包查询仍在一秒发出，但首次无回复的期限覆盖发送端五秒设备初始化，再给三秒
  // 控制往返。此绝对期限只计算一次；有首包后的恢复继续使用原三秒等待，不无限续期。
  const auto initialReplyDeadline = mediaReadyAt + deviceStartupWaitNs + controlWaitNs;
  const auto testDurationNs =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(options.testDuration).count());
  std::uint64_t mediaArrivalDeadline = 0;
  auto lastLoopNs = start;
  std::uint64_t maxLoopGapNs = 0;
  auto nextLocalOutputCheck = start;
  bool localOutputOverridden = false;
  std::uint64_t frame = 0;
  std::array<float, 480> tone{}, discard{};
  float decodedPeak = 0;
  // 诊断事件只在恢复进入、退出和失败时发出，与 Connected 界面事件分离。
  // 最大 worker 间隔覆盖上次诊断之后的整个窗口，不被每秒的界面统计清零。
  const auto reportDiagnostic = [&](const char *reason) {
    std::ostringstream text;
    std::uint64_t packets = 0;
    text << "audio-diagnostic reason=" << reason;
    if (receive) {
      const auto s = receiver->stats();
      packets = s.decodedPackets;
      text << " received=" << s.authenticatedPackets << " decoded=" << s.decodedPackets
           << " rejected=" << s.rejectedPackets << " lost=" << s.jitterLostPackets << " late=" << s.jitterLatePackets
           << " underrun=" << s.underrunFrames << " timeline=" << s.timelineRecoveries
           << " scheduler=" << s.schedulerRecoveries << " timeout=" << s.timeoutRecoveries
           << " recovery_epoch=" << s.recoveryEpoch;
    } else {
      packets = sender->stats().sentPackets;
      text << " sent=" << packets;
    }
    text << " worker_gap_ms=" << double(maxLoopGapNs) / 1e6;
    notify(observer, preview::SessionState::Diagnostic, text.str(), packets, reason);
    maxLoopGapNs = 0;
  };
  // 外层 runSession 捕获异常时引擎已销毁；在栈展开前保留最后的完整统计快照。
  // 记录失败本身不得妨碍声卡、静音保护和 SRTP 上下文的正常清理。
  struct FailureDiagnostic
  {
    const decltype(reportDiagnostic) &report;
    int exceptions = std::uncaught_exceptions();
    ~FailureDiagnostic() noexcept
    {
      try {
        if (std::uncaught_exceptions() > exceptions)
          report("session-failure");
      } catch (...) {
      }
    }
  } failureDiagnostic{reportDiagnostic};
  while (!stopping) {
    const auto now = ae::monotonicNowNs();
    const auto loopGapNs = now - lastLoopNs;
    lastLoopNs = now;
    maxLoopGapNs = std::max(maxLoopGapNs, loopGapNs);
    // 长时间未调度时先让引擎记录 worker 停顿，再排空积压 UDP；否则批交付旧包
    // 会先触发媒体偏移保护，掩盖本机调度停顿。正常轮询仍先收包，避免提前判丢。
    if (receive && loopGapNs > 40000000ULL)
      receiver->tick(now);
    if (now >= keyDeadline) {
      keyExpired = true;
      reportDiagnostic("media-key-lifetime");
      notify(
          observer, preview::SessionState::Renewing, "audio-session reason=media-key-lifetime; 正在更换媒体会话", 0,
          "media-key-lifetime"
      );
      break;
    }
    if (test && now - start >= testDurationNs)
      break;
    if (manageLocalOutput && now >= nextLocalOutputCheck) {
      std::wstring currentEndpoint;
      if (FAILED(aw::WasapiBackend::defaultRenderEndpointId(currentEndpoint)) || currentEndpoint != captureEndpoint)
        throw SessionFailure("audio-device-changed", "默认声卡已变化，正在恢复原设备并重新连接");
      const auto outputResult = localOutput.update(options.localPlayback->load());
      if (FAILED(outputResult))
        throw SessionFailure("local-playback-control-failed", "本机播放控制已中断，停止共享并恢复声音");
      if (outputResult == S_FALSE && !localOutputOverridden) {
        localOutputOverridden = true;
        notify(
            observer, preview::SessionState::LocalPlaybackOverridden, "本机声音已由系统或用户调整，本次共享尊重该设置"
        );
      }
      nextLocalOutputCheck = now + 100000000ULL;
    }
    for (unsigned work = 0; work < 32; ++work) {
      sockaddr_in from{};
      const auto bytes = network.receive(packet, from);
      if (!bytes)
        break;
      if (!sameEndpoint(peer, from))
        continue;
      if (establishedControl.verifyStop(std::span(packet.data(), bytes))) {
        notify(observer, preview::SessionState::Stopped, "audio-session reason=peer-stop");
        stopping = true;
        break;
      }
      preview::ControlType type{};
      preview::Nonce received{};
      if (preview::verifyControl(invite.value, std::span(packet.data(), bytes), type, received)) {
        if (received != nonce)
          continue;
        if (receive && type == preview::ControlType::Hello)
          (void)network.sendTo(preview::controlPacket(invite.value, preview::ControlType::Ack, nonce), peer);
      } else if (receive)
        (void)receiver->ingest(std::span(packet.data(), bytes), now);
    }
    // 只在非实时 worker 读取邮箱；UDP 的解密结果本身不能授予恢复权限。
    if (options.recovery) {
      if (const auto message = options.recovery->take(); message && message->request) {
        if (!receive && !message->response && message->request > answeredRequest)
          senderRequest = std::max(senderRequest, message->request);
        // 同一轮先处理 UDP：新媒体可能已完成恢复，此时延迟到达的进度回复必须忽略，
        // 否则发送位置的正常前进会被误判为媒体不可达并再次销毁已恢复的会话。
        const auto recoveryStats = receive ? receiver->stats() : ae::ReceiverStats{};
        if (receive && message->response && pendingRequest == message->request && !recoveryAuthorized &&
            pendingRecoveryEpoch == recoveryStats.recoveryEpoch &&
            (recoveryStats.requiresReanchor || !recoveryStats.authenticatedPackets)) {
          const bool sourceMoved = message->hasMedia && lastSourcePosition &&
                                   (message->sequence != lastSourcePosition->sequence ||
                                    message->timestamp != lastSourcePosition->timestamp);
          const bool sourceUnchanged = message->hasMedia && lastSourcePosition && !sourceMoved;
          // 进度回复可以先于 UDP 到达，位置前进只能启动有界宽限，不能立即认定不可达。
          // 尚未收到首包时，只要发送端明确已有媒体，也必须开始可达性计时。
          if (message->hasMedia && (sourceMoved || !recoveryStats.authenticatedPackets) && !mediaArrivalDeadline) {
            mediaArrivalDeadline = now + mediaArrivalGraceNs;
            reportDiagnostic("source-progress");
          }
          if (message->hasMedia) {
            const ae::MediaPosition position{message->sequence, message->timestamp};
            // 一次恢复只授权最初的位置；后续进度回复不能不断抬高接纳门槛，
            // 否则 TLS 比 UDP 快时会把正在途中、仍在原授权之后的合法媒体持续丢弃。
            // 首包前引擎尚未暂停，授权可能被拒绝；只有引擎明确接受了当前轮次才记录。
            // 同一批积压包内若先恢复再故障，新轮次必须重新取得匹配请求的可信回复。
            if (authorizedEpoch != recoveryStats.recoveryEpoch &&
                receiver->authorizeReanchor(position, pendingRecoveryEpoch)) {
              authorizedEpoch = pendingRecoveryEpoch;
              reportDiagnostic("recovery-authorized");
            }
            lastSourcePosition = position;
          }
          if ((!message->hasMedia || sourceUnchanged) && !waitingForMedia) {
            waitingForMedia = true;
            notify(observer, preview::SessionState::WaitingForMedia, "等待已配对电脑播放系统声音");
          }
          recoveryAuthorized = true;
          recoveryDeadline = now + controlWaitNs;
        }
      }
      if (!receive && senderRequest > answeredRequest) {
        const auto position = sender->mediaPosition();
        // 初始静默也必须回复。没有首个加密包与已经发送但 UDP 不可达是不同状态。
        options.onRecovery(
            {true, senderRequest, position ? position->sequence : std::uint16_t(0), position ? position->timestamp : 0,
             position.has_value()}
        );
        answeredRequest = senderRequest;
      }
    }
    if (test && !receive && !testCapture && now >= nextTone) {
      for (std::size_t i = 0; i < 240; ++i, ++frame)
        tone[2 * i] = tone[2 * i + 1] = .01f * std::sin(float(frame % 48000) * 6.2831853f * 440 / 48000);
      (void)sender->capture(tone, false, now);
      nextTone += 5000000;
      if (now > nextTone + 5000000)
        nextTone = now + 5000000;
    }
    if (receive) {
      receiver->tick(now);
      if (test && !testRender && now >= nextRender) {
        (void)receiver->render(discard);
        for (float sample : discard)
          decodedPeak = std::max(decodedPeak, std::abs(sample));
        nextRender += 5000000;
      }
      const auto stats = receiver->stats();
      // 引擎首次认证媒体前没有时间线，因此首包监测由已完成握手的 Session 负责。
      // 只有支持可信进度查询时才启动该检测，旧对端的初始静默不能被猜测成故障。
      const bool initialMediaMissing =
          options.recovery && !stats.authenticatedPackets && now - mediaReadyAt >= 1000000000ULL;
      if (stats.requiresReanchor || initialMediaMissing) {
        if (!options.recovery)
          throw SessionFailure(ae::recoveryReasonName(stats.recoveryReason), "声音暂时中断，正在重新连接");
        if (!pendingRequest || pendingRecoveryEpoch != stats.recoveryEpoch) {
          // 即使未观察到中间的“恢复完成”，轮次变化也会废弃旧回复、授权与到达宽限。
          // 新请求使用新编号；迟到的上一轮 TLS 回复不能跨故障授权新的时间线。
          pendingRequest = ++requestSerial;
          pendingRecoveryEpoch = stats.recoveryEpoch;
          recoveryAuthorized = false;
          authorizedEpoch.reset();
          waitingForMedia = false;
          mediaArrivalDeadline = 0;
          lastSourcePosition.reset();
          recoveryDeadline =
              initialMediaMissing ? std::max(now + controlWaitNs, initialReplyDeadline) : now + controlWaitNs;
          const auto recoveryReason =
              initialMediaMissing ? "initial-media-timeout" : ae::recoveryReasonName(stats.recoveryReason);
          reportDiagnostic(recoveryReason);
          std::ostringstream reason;
          reason << "audio-recovery reason=" << recoveryReason << " timeline=" << stats.timelineRecoveries
                 << " scheduler=" << stats.schedulerRecoveries << " timeout=" << stats.timeoutRecoveries
                 << " worker_gap_ms=" << double(loopGapNs) / 1e6;
          notify(observer, preview::SessionState::Recovering, reason.str(), 0, recoveryReason);
          options.onRecovery({false, pendingRequest});
        } else if (mediaArrivalDeadline && now >= mediaArrivalDeadline) {
          throw SessionFailure("media-unreachable", "声音传输暂时不可达，正在重新连接");
        } else if (now >= recoveryDeadline) {
          if (!recoveryAuthorized)
            throw SessionFailure("media-recovery-timeout", "声音恢复超时，正在重新连接");
          // 已收到授权却没有新媒体，最多每三秒查询一次发送进度，不调整 SRTP 上下文。
          recoveryAuthorized = false;
          pendingRequest = ++requestSerial;
          recoveryDeadline = now + controlWaitNs;
          options.onRecovery({false, pendingRequest});
        }
      } else if (pendingRequest) {
        pendingRequest = 0;
        recoveryAuthorized = false;
        authorizedEpoch.reset();
        waitingForMedia = false;
        mediaArrivalDeadline = 0;
        lastSourcePosition.reset();
        reportDiagnostic("recovered");
        notify(observer, preview::SessionState::Recovered, "audio-recovery reason=recovered; 已在原声卡上恢复媒体");
      }
      if (receiver->status().state == ae::EngineState::Failed)
        throw SessionFailure("unsupported-playback-quantum", "当前声卡的播放周期暂不受支持");
    } else if (sender->status().state == ae::EngineState::Failed)
      throw SessionFailure("audio-encoder-failed", "声音编码发送失败，正在重新连接");
    if (useDevice && (device.failed || device.generation.load() != device.active.load()))
      throw SessionFailure("audio-device-unavailable", "默认声卡已变化或不可用，正在重新连接");
    if (now >= nextReport) {
      std::ostringstream text;
      std::uint64_t packets = 0;
      if (receive) {
        const auto s = receiver->stats();
        packets = s.decodedPackets;
        text << "received=" << s.authenticatedPackets << " decoded=" << s.decodedPackets
             << " lost=" << s.jitterLostPackets << " late=" << s.jitterLatePackets << " underrun=" << s.underrunFrames
             << " timeline=" << s.timelineRecoveries << " scheduler=" << s.schedulerRecoveries
             << " timeout=" << s.timeoutRecoveries;
      } else {
        packets = sender->stats().sentPackets;
        text << "sent=" << packets;
      }
      text << " worker_gap_ms=" << double(maxLoopGapNs) / 1e6;
      // 恢复期间的统计仍保留，但不能把界面错误地更新为“正在收听”。
      if (!pendingRequest)
        notify(observer, preview::SessionState::Connected, text.str(), packets);
      nextReport = now + 1000000000ULL;
    }
    timer.waitMicroseconds(500);
  }
  if (backend)
    backend->stop();
  device.sender = nullptr;
  device.receiver = nullptr;
  if (sender)
    sender->stop();
  std::ostringstream finalText;
  std::uint64_t finalPackets = 0;
  if (receive) {
    const auto stats = receiver->stats();
    finalPackets = stats.decodedPackets;
    finalText << "final_decoded=" << stats.decodedPackets << " final_lost=" << stats.jitterLostPackets
              << " final_underrun=" << stats.underrunFrames
              << " device_render_callbacks=" << device.renderedCallbacks.load() << " test_decoded_peak=" << decodedPeak;
  } else {
    finalPackets = sender->stats().sentPackets;
    finalText << "final_sent=" << finalPackets << " device_capture_callbacks=" << device.capturedCallbacks.load();
  }
  notify(observer, preview::SessionState::Connected, finalText.str(), finalPackets);
  if (test && !stopping && !keyExpired) {
    if (testCapture && device.capturedCallbacks.load() < 100)
      throw std::runtime_error("真实系统声音测试未产生足够采集回调");
    if (testRender && device.renderedCallbacks.load() < 100)
      throw std::runtime_error("真实声卡测试未产生足够播放回调");
    if (receive && receiver->stats().decodedPackets < 600)
      throw std::runtime_error("双进程测试解码数量不足");
    if (!receive && sender->stats().sentPackets < 600)
      throw std::runtime_error("双进程测试发送数量不足");
  }
}
} // namespace
namespace preview {
void runSession(const SessionOptions &options, std::atomic<bool> &stop, const SessionObserver &observer)
{
  try {
    runInternal(options, stop, observer);
    notify(
        observer, SessionState::Stopped,
        options.invitationPath.empty() ? "音频共享已停止。" : "音频共享已停止。邀请文件是临时凭据，请删除两端副本。"
    );
  } catch (const SessionFailure &error) {
    notify(observer, SessionState::Failed, error.what(), 0, error.reason);
    throw;
  } catch (const std::exception &error) {
    notify(observer, SessionState::Failed, error.what(), 0, "session-exception");
    throw;
  } catch (...) {
    notify(observer, SessionState::Failed, "音频共享发生未知错误");
    throw;
  }
}
} // namespace preview
