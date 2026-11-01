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
void notify(
    const preview::SessionObserver &observer, preview::SessionState state, const std::string &message,
    std::uint64_t packets = 0
) noexcept
{
  if (!observer)
    return;
  try {
    observer({state, message, packets});
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
      throw std::runtime_error("UDP receive failed");
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
  if (options.handshakeTimeout <= std::chrono::milliseconds::zero() ||
      options.handshakeTimeout > std::chrono::minutes(1))
    throw std::invalid_argument("handshakeTimeout must be between 1 and 60000 milliseconds");
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
      throw std::runtime_error(
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
    throw std::runtime_error("音频协商凭据已过期。请重新开启音频共享。");
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
  } stopGuard{network, peer, preview::controlPacket(invite.value, preview::ControlType::Stop, nonce)};
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
    const auto until = ae::monotonicNowNs() + 5000000000ULL;
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
    receiver = std::make_unique<ae::AudioReceiver>(format, 5, config, std::move(key), ae::ReceiverOptions{.35f});
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
  if (receive)
    (void)network.sendTo(preview::controlPacket(invite.value, preview::ControlType::Ack, nonce), peer);
  notify(
      observer, preview::SessionState::Connected,
      receive ? (test && !testRender ? "已连接：测试模式仅解码丢弃，不播放。"
                                     : "已连接：正在播放远端系统声音（初始音量35%）。")
              : "已连接：正在发送系统声音，请在本机播放音乐。"
  );
  auto start = ae::monotonicNowNs(), nextTone = start, nextRender = start, nextReport = start;
  auto nextLocalOutputCheck = start;
  bool localOutputOverridden = false;
  std::uint64_t frame = 0;
  std::array<float, 480> tone{}, discard{};
  float decodedPeak = 0;
  while (!stopping && preview::unixSeconds() < invite.value.expiresUnixSeconds) {
    const auto now = ae::monotonicNowNs();
    if (test && now - start >= 5000000000ULL)
      break;
    if (manageLocalOutput && now >= nextLocalOutputCheck) {
      std::wstring currentEndpoint;
      if (FAILED(aw::WasapiBackend::defaultRenderEndpointId(currentEndpoint)) || currentEndpoint != captureEndpoint)
        throw std::runtime_error("默认声卡已变化，正在恢复原设备并重新连接");
      const auto outputResult = localOutput.update(options.localPlayback->load());
      if (FAILED(outputResult))
        throw std::runtime_error("本机播放控制已中断，停止共享并恢复声音");
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
      preview::ControlType type{};
      preview::Nonce received{};
      if (preview::verifyControl(invite.value, std::span(packet.data(), bytes), type, received)) {
        if (received != nonce)
          continue;
        if (type == preview::ControlType::Stop) {
          stopping = true;
          break;
        }
        if (receive && type == preview::ControlType::Hello)
          (void)network.sendTo(preview::controlPacket(invite.value, preview::ControlType::Ack, nonce), peer);
      } else if (receive)
        (void)receiver->ingest(std::span(packet.data(), bytes), now);
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
      if (receiver->stats().requiresReanchor)
        throw std::runtime_error("音频中断超过恢复范围。请重新开启音频共享以建立新会话。");
      if (receiver->status().state == ae::EngineState::Failed)
        throw std::runtime_error("播放周期不支持：当前预览要求声卡回调不超过10ms。");
    } else if (sender->status().state == ae::EngineState::Failed)
      throw std::runtime_error("编码发送失败");
    if (useDevice && (device.failed || device.generation.load() != device.active.load()))
      throw std::runtime_error("默认声卡变化或不可用。请重新启动预览。");
    if (now >= nextReport) {
      std::ostringstream text;
      std::uint64_t packets = 0;
      if (receive) {
        const auto s = receiver->stats();
        packets = s.decodedPackets;
        text << "received=" << s.authenticatedPackets << " decoded=" << s.decodedPackets
             << " lost=" << s.jitterLostPackets << " underrun=" << s.underrunFrames;
      } else {
        packets = sender->stats().sentPackets;
        text << "sent=" << packets;
      }
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
  if (test) {
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
  } catch (const std::exception &error) {
    notify(observer, SessionState::Failed, error.what());
    throw;
  } catch (...) {
    notify(observer, SessionState::Failed, "音频共享发生未知错误");
    throw;
  }
}
} // namespace preview
