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
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace ae = deskflow::audio::engine;
namespace aw = deskflow::audio::windows;
namespace at = deskflow::audio::transport;
using deskflow::audio::dsp::PcmFormat;
namespace preview {
struct LocalOutputState
{
  aw::MuteWatchdog watchdog;
  std::wstring endpoint;
  HRESULT prepare(const std::wstring &id, bool playLocally)
  {
    if (endpoint != id || !watchdog.running()) {
      const auto stopped = watchdog.stop();
      if (FAILED(stopped))
        return stopped;
      endpoint.clear();
      const auto started = watchdog.start(id);
      if (FAILED(started))
        return started;
      endpoint = id;
    }
    return watchdog.update(playLocally);
  }
};
std::shared_ptr<LocalOutputState> createLocalOutputState()
{
  return std::make_shared<LocalOutputState>();
}
bool restoreLocalOutputState(const std::shared_ptr<LocalOutputState> &state) noexcept
{
  return !state || SUCCEEDED(state->watchdog.stop());
}
} // namespace preview
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
// Normalize IPv4 into mapped IPv6 on dual-stack sockets, preserving endpoint identity.
struct Endpoint
{
  sockaddr_storage address{};
  int size = 0;
};
bool parseEndpoint(const std::string &text, std::uint16_t port, bool dualStack, Endpoint &out)
{
  IN_ADDR ipv4{};
  IN6_ADDR ipv6{};
  if (InetPtonA(AF_INET, text.c_str(), &ipv4) == 1) {
    if (!dualStack) {
      auto &a = *reinterpret_cast<sockaddr_in *>(&out.address);
      a.sin_family = AF_INET;
      a.sin_port = htons(port);
      a.sin_addr = ipv4;
      out.size = sizeof(a);
    } else {
      auto &a = *reinterpret_cast<sockaddr_in6 *>(&out.address);
      a.sin6_family = AF_INET6;
      a.sin6_port = htons(port);
      a.sin6_addr.u.Byte[10] = a.sin6_addr.u.Byte[11] = 0xff;
      std::memcpy(&a.sin6_addr.u.Byte[12], &ipv4, sizeof(ipv4));
      out.size = sizeof(a);
    }
    return true;
  }
  if (!dualStack || InetPtonA(AF_INET6, text.c_str(), &ipv6) != 1 || IN6_IS_ADDR_LINKLOCAL(&ipv6))
    return false; // A remote interface index is not meaningful on this host.
  auto &a = *reinterpret_cast<sockaddr_in6 *>(&out.address);
  a.sin6_family = AF_INET6;
  a.sin6_port = htons(port);
  a.sin6_addr = ipv6;
  out.size = sizeof(a);
  return true;
}
bool sameAddress(const Endpoint &a, const Endpoint &b)
{
  if (a.address.ss_family != b.address.ss_family)
    return false;
  if (a.address.ss_family == AF_INET)
    return reinterpret_cast<const sockaddr_in *>(&a.address)->sin_addr.s_addr ==
           reinterpret_cast<const sockaddr_in *>(&b.address)->sin_addr.s_addr;
  const auto &x = *reinterpret_cast<const sockaddr_in6 *>(&a.address);
  const auto &y = *reinterpret_cast<const sockaddr_in6 *>(&b.address);
  return std::memcmp(&x.sin6_addr, &y.sin6_addr, sizeof(x.sin6_addr)) == 0 && x.sin6_scope_id == y.sin6_scope_id;
}
bool sameEndpoint(const Endpoint &a, const Endpoint &b)
{
  if (!sameAddress(a, b))
    return false;
  if (a.address.ss_family == AF_INET)
    return reinterpret_cast<const sockaddr_in *>(&a.address)->sin_port ==
           reinterpret_cast<const sockaddr_in *>(&b.address)->sin_port;
  return reinterpret_cast<const sockaddr_in6 *>(&a.address)->sin6_port ==
         reinterpret_cast<const sockaddr_in6 *>(&b.address)->sin6_port;
}
struct Network
{
  SOCKET socketHandle = INVALID_SOCKET;
  bool dualStack = false;
  Network()
  {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
      throw std::runtime_error("WSAStartup failed");
    socketHandle = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle != INVALID_SOCKET) {
      DWORD disabled = 0;
      if (setsockopt(socketHandle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&disabled), sizeof(disabled)) == 0)
        dualStack = true;
      else {
        closesocket(socketHandle);
        socketHandle = INVALID_SOCKET;
      }
    }
    if (!dualStack)
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
    Endpoint a;
    if (dualStack) {
      auto &ip = *reinterpret_cast<sockaddr_in6 *>(&a.address);
      ip.sin6_family = AF_INET6;
      a.size = sizeof(ip);
    } else {
      auto &ip = *reinterpret_cast<sockaddr_in *>(&a.address);
      ip.sin_family = AF_INET;
      a.size = sizeof(ip);
    }
    if (bind(socketHandle, reinterpret_cast<sockaddr *>(&a.address), a.size) ||
        getsockname(socketHandle, reinterpret_cast<sockaddr *>(&a.address), &a.size))
      throw std::runtime_error("Cannot bind or query receiver port");
    return dualStack ? ntohs(reinterpret_cast<sockaddr_in6 *>(&a.address)->sin6_port)
                     : ntohs(reinterpret_cast<sockaddr_in *>(&a.address)->sin_port);
  }
  bool sendTo(std::span<const std::uint8_t> bytes, const Endpoint &a) noexcept
  {
    return sendto(socketHandle, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
                  reinterpret_cast<const sockaddr *>(&a.address), a.size) == static_cast<int>(bytes.size());
  }
  int receive(std::span<std::uint8_t> bytes, Endpoint &from)
  {
    from.size = sizeof(from.address);
    const int n = recvfrom(socketHandle, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
                           reinterpret_cast<sockaddr *>(&from.address), &from.size);
    if (n == SOCKET_ERROR) {
      const auto error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK || error == WSAEMSGSIZE || error == WSAECONNRESET)
        return 0;
      throw std::runtime_error("UDP receive failed");
    }
    return n;
  }
};
std::vector<std::string> addresses()
{
  ULONG size = 16384;
  std::vector<std::uint8_t> memory(size);
  auto *head = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data());
  constexpr auto flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  auto result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, head, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    memory.resize(size);
    head = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data());
    result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, head, &size);
  }
  std::vector<std::string> out;
  if (result == NO_ERROR)
    for (auto *a = head; a && out.size() < 8; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        continue;
      for (auto *u = a->FirstUnicastAddress; u && out.size() < 8; u = u->Next) {
        auto *address = u->Address.lpSockaddr;
        char text[INET6_ADDRSTRLEN]{};
        const void *ip = nullptr;
        if (address->sa_family == AF_INET)
          ip = &reinterpret_cast<sockaddr_in *>(address)->sin_addr;
        else if (address->sa_family == AF_INET6) {
          const auto &v6 = reinterpret_cast<sockaddr_in6 *>(address)->sin6_addr;
          if (IN6_IS_ADDR_LINKLOCAL(&v6))
            continue;
          ip = &v6;
        }
        if (ip && InetNtopA(address->sa_family, const_cast<void *>(ip), text, sizeof(text)) &&
            std::find(out.begin(), out.end(), text) == out.end())
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
  std::atomic<std::uint32_t> rate{0}, channels{0}, channelMask{0};
  std::atomic<bool> failed{false};
  static void status(void *p, const aw::Status &s) noexcept
  {
    auto &d = *static_cast<Device *>(p);
    if (s.state == aw::State::Running) {
      d.rate = s.format.sampleRate;
      d.channels = s.format.channels;
      d.channelMask = s.format.channelMask;
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
  if (options.sharedPlayback && (!receive || options.playbackSlot >= 2))
    throw std::invalid_argument("Shared playback requires a receiving slot 0 or 1");
  if (test && (!std::isfinite(options.testToneHz) || options.testToneHz < 0 || options.testToneHz >= 24000))
    throw std::invalid_argument("Invalid synthetic test frequency");
  if (test &&
      (options.testDuration <= std::chrono::milliseconds::zero() || options.testDuration > std::chrono::minutes(10)))
    throw std::invalid_argument("testDuration must be between 1 and 600000 milliseconds");
  if (options.handshakeTimeout <= std::chrono::milliseconds::zero() ||
      options.handshakeTimeout > std::chrono::minutes(1))
    throw std::invalid_argument("handshakeTimeout must be between 1 and 60000 milliseconds");
  if (options.firstMediaTimeout <= std::chrono::milliseconds::zero() ||
      options.firstMediaTimeout > std::chrono::minutes(1))
    throw std::invalid_argument("firstMediaTimeout must be between 1 and 60000 milliseconds");
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
  if (!test && !options.sharedPlayback) {
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
  Endpoint peer{};
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
  while (!connected && !stopping &&
         (receive ? preview::unixSeconds() < invite.value.expiresUnixSeconds
                  : preview::validInvitationExpiry(invite.value.expiresUnixSeconds, preview::unixSeconds()))) {
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
        Endpoint target{};
        if (parseEndpoint(address, invite.value.port, network.dualStack, target))
          (void)network.sendTo(hello, target);
      }
      nextHello = now + 500000000ULL;
    }
    Endpoint from{};
    const auto bytes = network.receive(packet, from);
    if (bytes) {
      preview::ControlType type{};
      preview::Nonce received{};
      if (preview::verifyControl(invite.value, std::span(packet.data(), bytes), type, received)) {
        if (receive && type == preview::ControlType::Hello) {
          if (!options.expectedSenderAddress.empty()) {
            Endpoint expected{};
            if (!parseEndpoint(options.expectedSenderAddress, 0, network.dualStack, expected) ||
                !sameAddress(expected, from))
              continue;
          }
          peer = from;
          nonce = received;
          connected = true;
        }
        if (!receive && type == preview::ControlType::Ack && received == nonce) {
          bool expected = false;
          for (const auto &address : invite.value.addresses) {
            Endpoint target{};
            if (parseEndpoint(address, invite.value.port, network.dualStack, target) && sameEndpoint(target, from))
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
    Endpoint peer;
    std::vector<std::uint8_t> packet;
    bool enabled;
    ~StopGuard()
    {
      for (int i = 0; enabled && i < 3; ++i)
        (void)network.sendTo(packet, peer);
    }
  } stopGuard{
      network, peer, preview::controlPacket(invite.value, preview::ControlType::Stop, nonce),
      !options.controlOwnsLifetime
  };
  const auto ack = preview::controlPacket(invite.value, preview::ControlType::Ack, nonce);
  auto session = preview::sessionMediaKey(invite.value, nonce);
  at::SrtpKey key(session);
  SecureZeroMemory(session.data(), session.size());
  at::StreamConfig config{invite.value.ssrc, 1, 48000, 2, 240};
  Device device;
  std::unique_ptr<ae::AudioSender> sender;
  std::unique_ptr<ae::AudioReceiver> receiver;
  preview::LocalOutputState sessionOutput; // CLI restores on exit; GUI can retain ownership across retries.
  auto &localOutput = options.localOutputState ? *options.localOutputState : sessionOutput;
  struct PlaybackRegistration
  {
    std::shared_ptr<preview::SharedPlayback> output;
    std::size_t slot;
    bool attached = false;
    ~PlaybackRegistration()
    {
      if (output && attached)
        output->detach(slot);
    }
  } registration{options.sharedPlayback, options.playbackSlot};
  std::unique_ptr<aw::WasapiBackend> backend; // Stops callbacks before destroying engines.
  PcmFormat format{48000, 2};
  const bool useDevice = !test || testRender || testCapture;
  const bool manageLocalOutput = !receive && useDevice && options.localPlayback;
  std::wstring captureEndpoint;
  if (manageLocalOutput && FAILED(aw::WasapiBackend::defaultRenderEndpointId(captureEndpoint)))
    throw std::runtime_error("无法读取共享音频设备，本机静音未修改");
  if (options.sharedPlayback) {
    const auto prepared = options.sharedPlayback->prepare(stopping);
    if (!prepared)
      return;
    format = *prepared;
  }
  else if (useDevice) {
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
    format = {device.rate.load(), device.channels.load(), device.channelMask.load()};
    device.active = device.generation.load();
  }
  if (receive) {
    receiver = std::make_unique<ae::AudioReceiver>(format, 5, config, std::move(key), ae::ReceiverOptions{.35f});
    device.receiver = receiver.get();
    if (options.sharedPlayback) {
      options.sharedPlayback->attach(options.playbackSlot, receiver.get());
      registration.attached = true;
    }
  } else {
    sender = std::make_unique<ae::AudioSender>(format, 5, config, std::move(key), [&](auto bytes) {
      return network.sendTo(bytes, peer);
    });
    sender->start();
    device.sender = sender.get();
  }
  if (manageLocalOutput) {
    if (FAILED(localOutput.prepare(captureEndpoint, options.localPlayback->load())))
      throw std::runtime_error("无法安全控制本机播放，已暂停音频共享");
  }
  if (receive)
    (void)network.sendTo(ack, peer);
  notify(
      observer, receive ? preview::SessionState::Waiting : preview::SessionState::Connected,
      receive ? "音频协商完成，等待远端声音..."
              : "已连接：正在发送系统声音，请在本机播放音乐。"
  );
  auto start = ae::monotonicNowNs(), nextTone = start, nextRender = start, nextReport = start;
  auto nextLocalOutputCheck = start;
  bool localOutputOverridden = false;
  std::uint64_t frame = 0;
  std::array<float, 480> tone{}, discard{};
  float decodedPeak = 0;
  // Credential expiry limits NEW handshakes. An authenticated active stream
  // remains owned by the paired control connection until stop/disconnect.
  while (!stopping) {
    const auto now = ae::monotonicNowNs();
    if (test && now - start >= static_cast<std::uint64_t>(options.testDuration.count()) * 1000000ULL)
      break;
    if (manageLocalOutput && now >= nextLocalOutputCheck) {
      std::wstring currentEndpoint;
      if (FAILED(aw::WasapiBackend::defaultRenderEndpointId(currentEndpoint)) || currentEndpoint != captureEndpoint)
        throw std::runtime_error("默认声卡已变化，正在重新连接");
      const auto outputResult = localOutput.watchdog.update(options.localPlayback->load());
      if (FAILED(outputResult))
        throw std::runtime_error("本机播放控制已中断，已暂停音频共享");
      if (outputResult == S_FALSE && !localOutputOverridden) {
        localOutputOverridden = true;
        notify(
            observer, preview::SessionState::LocalPlaybackOverridden, "本机声音已由系统或用户调整，本次共享尊重该设置"
        );
      }
      nextLocalOutputCheck = now + 100000000ULL;
    }
    for (unsigned work = 0; work < 32; ++work) {
      Endpoint from{};
      const auto bytes = network.receive(packet, from);
      if (!bytes)
        break;
      if (!sameEndpoint(peer, from))
        continue;
      preview::ControlType type{};
      preview::Nonce received{};
      if (preview::verifyControl(invite.value, std::span(packet.data(), bytes), type, received, false)) {
        if (received != nonce)
          continue;
        if (type == preview::ControlType::Stop) {
          stopping = true;
          break;
        }
        if (receive && type == preview::ControlType::Hello)
          (void)network.sendTo(ack, peer);
      } else if (receive)
        (void)receiver->ingest(std::span(packet.data(), bytes), now);
    }
    if (test && !receive && !testCapture && now >= nextTone) {
      for (std::size_t i = 0; i < 240; ++i, ++frame)
        tone[2 * i] = tone[2 * i + 1] =
            options.testToneHz == 0
                ? .01f
                : .01f * std::sin(float(frame % 48000) * 6.2831853f * float(options.testToneHz) / 48000);
      (void)sender->capture(tone, false, now);
      nextTone += 5000000;
      if (now > nextTone + 5000000)
        nextTone = now + 5000000;
    }
    if (receive) {
      if (options.playbackControl) {
        receiver->setVolume(options.playbackControl->volume.load());
        receiver->setMuted(options.playbackControl->muted.load());
      }
      receiver->tick(now);
      if (receiver->stats().decodedPackets == 0 &&
          now - start >= static_cast<std::uint64_t>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(options.firstMediaTimeout).count()
                         ))
        throw std::runtime_error("等待首个可解码音频数据包超时，需要重新建立会话");
      if (test && !testRender && !options.sharedPlayback && now >= nextRender) {
        (void)receiver->render(discard);
        for (float sample : discard)
          decodedPeak = std::max(decodedPeak, std::abs(sample));
        nextRender += 5000000;
      }
      if (receiver->stats().requiresReanchor)
        throw std::runtime_error("音频中断超过恢复范围，需要重新建立会话");
      if (receiver->status().state == ae::EngineState::Failed)
        throw std::runtime_error("播放周期不支持：当前预览要求声卡回调不超过10ms。");
    } else if (sender->status().state == ae::EngineState::Failed)
      throw std::runtime_error("编码发送失败");
    if (useDevice && (device.failed || device.generation.load() != device.active.load()))
      throw std::runtime_error("声卡变化或暂时不可用，需要重新建立会话");
    if (options.sharedPlayback && !options.sharedPlayback->healthy())
      throw std::runtime_error("收听设备变化或暂时不可用，需要重新建立会话");
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
      notify(observer, receive && !packets ? preview::SessionState::Waiting : preview::SessionState::Connected,
             text.str(), packets);
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
  notify(observer, receive && !finalPackets ? preview::SessionState::Waiting : preview::SessionState::Connected,
         finalText.str(), finalPackets);
  if (test) {
    if (testCapture && device.capturedCallbacks.load() < 100)
      throw std::runtime_error("真实系统声音测试未产生足够采集回调");
    if (testRender && !options.sharedPlayback && device.renderedCallbacks.load() < 100)
      throw std::runtime_error("真实声卡测试未产生足够播放回调");
    if (receive && receiver->stats().decodedPackets < 600)
      throw std::runtime_error("双进程测试解码数量不足");
    if (!receive && sender->stats().sentPackets < 600)
      throw std::runtime_error("双进程测试发送数量不足");
  }
}
} // namespace
namespace preview {
struct SharedPlayback::Impl
{
  const bool test, testRender;
  std::timed_mutex setup;
  Device device;
  std::array<std::atomic<ae::AudioReceiver *>, 2> sources{};
  std::atomic<unsigned> readers{0};
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> callbacks{0};
  std::atomic<std::uint64_t> twoSource{0};
  std::atomic<float> maximum{0};
  std::unique_ptr<aw::WasapiBackend> backend;
  std::thread synthetic;
  HANDLE exclusive = nullptr;
  PcmFormat format{48000, 2};
  bool prepared = false;
  Impl(bool testing, bool render) : test(testing), testRender(render)
  {
  }
  ~Impl()
  {
    stopping = true;
    backend.reset();
    if (synthetic.joinable())
      synthetic.join();
    if (exclusive)
      CloseHandle(exclusive);
  }
  static void status(void *context, const aw::Status &value) noexcept
  {
    Device::status(&static_cast<Impl *>(context)->device, value);
  }
  static void render(void *context, std::span<float> output, const aw::DeviceFormat &format) noexcept
  {
    auto &self = *static_cast<Impl *>(context);
    self.readers.fetch_add(1);
    std::array<ae::AudioReceiver *, 2> active{};
    std::size_t count = 0;
    if (format.generation == self.device.active.load()) {
      for (auto &source : self.sources)
        if (auto *receiver = source.load())
          active[count++] = receiver;
    }
    (void)ae::AudioReceiver::mix(std::span<ae::AudioReceiver *const>(active.data(), count), output);
    self.readers.fetch_sub(1);
    float peak = self.maximum.load(std::memory_order_relaxed);
    for (auto sample : output)
      peak = std::max(peak, std::abs(sample));
    self.maximum.store(peak, std::memory_order_relaxed);
    self.callbacks.fetch_add(1, std::memory_order_relaxed);
    if (count == 2)
      self.twoSource.fetch_add(1, std::memory_order_relaxed);
  }
  bool healthy() const noexcept
  {
    return !device.failed.load() && device.generation.load() == device.active.load();
  }
};
SharedPlayback::SharedPlayback(bool test, bool testRender) : d(std::make_unique<Impl>(test, testRender))
{
}
SharedPlayback::~SharedPlayback() = default;
std::optional<PcmFormat> SharedPlayback::prepare(const std::atomic<bool> &stop)
{
  std::unique_lock lock(d->setup, std::defer_lock);
  while (!stop.load() && !lock.try_lock_for(std::chrono::milliseconds(10))) {}
  if (stop.load())
    return std::nullopt;
#ifdef DESKFLOW_SESSION_TEST_PREPARE_HOOK
  sessionPrepareHook(stop);
  if (stop.load())
    return std::nullopt;
#endif
  if (d->prepared && d->healthy())
    return d->format;
  for (auto &source : d->sources)
    if (source.load())
      throw std::runtime_error("正在等待音频来源释放旧播放设备");
  d->backend.reset();
  d->prepared = false;
  d->device.failed = false;
  d->device.generation = 0;
  d->device.active = 0;
  if (!d->test && !d->exclusive) {
    const auto handle = CreateMutexW(nullptr, FALSE, L"Local\\DeskflowAudioPreviewActive");
    if (!handle || GetLastError() == ERROR_ALREADY_EXISTS) {
      if (handle)
        CloseHandle(handle);
      throw std::runtime_error("本Windows会话已经运行另一音频共享");
    }
    d->exclusive = handle;
  }
  if (!d->test || d->testRender) {
    aw::Callbacks callbacks{d.get(), nullptr, Impl::render, Impl::status};
    d->backend = std::make_unique<aw::WasapiBackend>(aw::Direction::Render, callbacks);
    if (!d->backend->start())
      throw std::runtime_error("无法启动收听设备");
    const auto until = ae::monotonicNowNs() + 5000000000ULL;
    while (!stop.load() && !d->device.generation && !d->device.failed && ae::monotonicNowNs() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop.load())
      return std::nullopt;
    if (!d->device.generation)
      throw std::runtime_error("5秒内未打开收听设备");
    d->format = {d->device.rate.load(), d->device.channels.load(), d->device.channelMask.load()};
    d->device.active = d->device.generation.load();
  } else if (!d->synthetic.joinable()) {
    d->device.generation = 1;
    d->device.active = 1;
    d->synthetic = std::thread([impl = d.get()] {
      std::array<float, 480> output{};
      aw::DeviceFormat format{48000, 2, 0, 240, 1};
      aw::WorkerTimer timer;
      auto next = ae::monotonicNowNs();
      while (!impl->stopping) {
        const auto now = ae::monotonicNowNs();
        if (now >= next) {
          Impl::render(impl, output, format);
          next = now + 5000000ULL;
        }
        timer.waitMicroseconds(500);
      }
    });
  }
  d->prepared = true;
  return d->format;
}
void SharedPlayback::attach(std::size_t slot, ae::AudioReceiver *receiver)
{
  std::lock_guard lock(d->setup);
  if (slot >= d->sources.size() || !receiver || d->sources[slot].load() || receiver->outputFormat() != d->format)
    throw std::invalid_argument("Invalid or occupied shared playback slot");
  d->sources[slot].store(receiver);
}
void SharedPlayback::detach(std::size_t slot) noexcept
{
  if (slot >= d->sources.size())
    return;
  // Seq-cst publication + reader entry ensures every callback holding the old
  // pointer finishes before its source worker destroys that receiver.
  d->sources[slot].store(nullptr);
  while (d->readers.load() != 0)
    std::this_thread::yield();
}
bool SharedPlayback::healthy() const noexcept
{
  return d->healthy();
}
std::uint64_t SharedPlayback::renderedCallbacks() const noexcept
{
  return d->callbacks.load();
}
std::uint64_t SharedPlayback::twoSourceCallbacks() const noexcept
{
  return d->twoSource.load();
}
float SharedPlayback::peak() const noexcept
{
  return d->maximum.load();
}
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
