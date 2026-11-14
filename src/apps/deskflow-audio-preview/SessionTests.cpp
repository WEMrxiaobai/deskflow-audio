/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Loopback-only session regression: synthetic tone, no device access or secret files.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Session.h"
#include "audio/codec/OpusCodec.h"
#include "audio/engine/AudioEngine.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&function)
{
  bool rejected = false;
  try {
    function();
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "Invalid session unexpectedly succeeded");
}
struct Offer
{
  preview::Invitation value;
  ~Offer()
  {
    preview::erase(value);
  }
};
preview::SessionOptions memoryReceiver()
{
  preview::SessionOptions options;
  options.receive = true;
  options.test = true;
  options.onReady = [](const auto &) {};
  return options;
}
void validation()
{
  std::atomic<bool> stop{false};
  rejects([&] { preview::runSession({}, stop); });
  auto receiver = memoryReceiver();
  receiver.onReady = {};
  rejects([&] { preview::runSession(receiver, stop); });
  receiver = memoryReceiver();
  receiver.invitation.emplace();
  rejects([&] { preview::runSession(receiver, stop); });
  receiver = memoryReceiver();
  receiver.handshakeTimeout = 0ms;
  rejects([&] { preview::runSession(receiver, stop); });
  receiver.handshakeTimeout = 60001ms;
  rejects([&] { preview::runSession(receiver, stop); });
  receiver = memoryReceiver();
  receiver.testDuration = 0ms;
  rejects([&] { preview::runSession(receiver, stop); });
  receiver.testDuration = 60001ms;
  rejects([&] { preview::runSession(receiver, stop); });
  // 可控时钟只能用于纯合成测试，防止正式共享或真实设备被测试时间替代。
  receiver = memoryReceiver();
  receiver.testClock = [](const auto &) { return std::uint64_t(1); };
  receiver.test = false;
  rejects([&] { preview::runSession(receiver, stop); });
  receiver.test = true;
  receiver.testRender = true;
  rejects([&] { preview::runSession(receiver, stop); });
  preview::SessionOptions sender;
  sender.test = true;
  sender.invitation.emplace();
  sender.invitationPath = L"must-not-open.dshaudio";
  rejects([&] { preview::runSession(sender, stop); });
  sender.invitationPath.clear();
  sender.onReady = [](const auto &) {};
  rejects([&] { preview::runSession(sender, stop); });
  sender.onReady = {};
  rejects([&] { preview::runSession(sender, stop); }); // Invalid in-memory offer validates before sending.
  receiver = memoryReceiver();
  int ready = 0;
  receiver.onReady = [&](const auto &) { ++ready; };
  stop = true;
  preview::runSession(receiver, stop);
  check(ready == 0, "Pre-stopped receiver published an offer");
}
void deadlineAndCallbacks()
{
  auto receiver = memoryReceiver();
  receiver.handshakeTimeout = 80ms;
  std::atomic<bool> stop{false};
  int ready = 0, failed = 0, connected = 0;
  receiver.onReady = [&](const auto &offer) {
    ++ready;
    check(
        offer.port != 0 && offer.ssrc != 0 && offer.addresses == std::vector<std::string>{"127.0.0.1"},
        "Receiver did not publish its bound loopback offer"
    );
  };
  auto start = std::chrono::steady_clock::now();
  rejects([&] {
    preview::runSession(receiver, stop, [&](const auto &event) {
      failed += event.state == preview::SessionState::Failed;
      connected += event.state == preview::SessionState::Connected;
    });
  });
  check(ready == 1 && failed == 1 && connected == 0, "Handshake deadline reporting failed");
  check(std::chrono::steady_clock::now() - start < 2s, "Receiver waited for credential expiry instead of deadline");
  Offer offer;
  receiver.onReady = [&](const auto &value) {
    offer.value = value;
    throw std::runtime_error("Offer delivery failed");
  };
  rejects([&] { preview::runSession(receiver, stop); });
  // Closed receiver socket: sender must obey its own deadline, not the offer's 10-minute expiry.
  preview::SessionOptions sender;
  sender.test = true;
  sender.invitation = offer.value;
  sender.handshakeTimeout = 80ms;
  start = std::chrono::steady_clock::now();
  rejects([&] { preview::runSession(sender, stop); });
  preview::erase(*sender.invitation);
  check(std::chrono::steady_clock::now() - start < 2s, "Sender ignored handshake deadline");
  receiver.onReady = [&](const auto &) { stop = true; };
  // Ordinary reporting exceptions must not prevent cooperative teardown.
  preview::runSession(receiver, stop, [](const auto &) { throw std::runtime_error("Reporting failed"); });
}
void cooperativeWaitingStop()
{
  auto receiver = memoryReceiver();
  std::atomic<bool> stop{false};
  std::promise<void> ready;
  auto published = ready.get_future();
  receiver.onReady = [&](const auto &) { ready.set_value(); };
  auto worker = std::async(std::launch::async, [&] { preview::runSession(receiver, stop); });
  if (published.wait_for(2s) != std::future_status::ready) {
    stop = true;
    worker.get();
    throw std::runtime_error("Receiver never became ready");
  }
  const auto start = std::chrono::steady_clock::now();
  stop = true;
  const auto status = worker.wait_for(500ms);
  worker.get();
  check(
      status == std::future_status::ready && std::chrono::steady_clock::now() - start < 500ms,
      "Waiting receiver did not stop cooperatively"
  );
}
void noOverlap()
{
  auto receiver = memoryReceiver();
  receiver.test = false; // Exercises real-session exclusion without opening an audio device.
  std::atomic<bool> stop{false}, otherStop{false};
  bool rejected = false, otherReady = false;
  receiver.onReady = [&](const auto &) {
    auto other = receiver;
    other.onReady = [&](const auto &) {
      otherReady = true;
      otherStop = true;
    };
    try {
      preview::runSession(other, otherStop);
    } catch (const std::exception &) {
      rejected = true;
    }
    stop = true;
  };
  preview::runSession(receiver, stop);
  check(rejected && !otherReady, "Overlapping real session was allowed to publish an offer");
  // The exclusive handle must be released after cooperative stop.
  stop = false;
  receiver.onReady = [&](const auto &) { stop = true; };
  preview::runSession(receiver, stop);
}
void rejectsUnauthenticatedHello()
{
  auto receiver = memoryReceiver();
  std::atomic<bool> stop{false};
  bool connected = false;
  receiver.handshakeTimeout = 120ms;
  receiver.onReady = [&](const auto &value) {
    Offer corrupt{value};
    corrupt.value.controlKey[0] ^= 1;
    const auto hello = preview::controlPacket(corrupt.value, preview::ControlType::Hello, preview::randomNonce());
    const auto socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check(socketHandle != INVALID_SOCKET, "Cannot create untrusted test sender");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(value.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto bytes = sendto(
        socketHandle, reinterpret_cast<const char *>(hello.data()), static_cast<int>(hello.size()), 0,
        reinterpret_cast<const sockaddr *>(&address), sizeof(address)
    );
    closesocket(socketHandle);
    check(bytes == static_cast<int>(hello.size()), "Cannot send unauthenticated Hello");
  };
  rejects([&] {
    preview::runSession(receiver, stop, [&](const auto &event) {
      connected |= event.state == preview::SessionState::Connected;
    });
  });
  check(!connected, "Receiver accepted a Hello without the authenticated offer secret");
}
// 合成音频、回环 UDP 与可信回调邮箱，不访问声卡；故意暂停接收 worker 后检验同会话恢复。
void memoryPair(bool recover = false, bool answerRecovery = true, bool renewKey = false, bool capturePauses = false)
{
  auto receiver = memoryReceiver();
  std::atomic<bool> receiverStop{false}, senderStop{false};
  std::promise<Offer> ready;
  auto published = ready.get_future();
  std::uint64_t received = 0, sent = 0;
  int readyCalls = 0;
  int recovering = 0, recovered = 0, renewing = 0;
  bool paused = false;
  bool recoveryTimedOut = false;
  bool schedulerReported = false;
  bool timelineReported = false;
  bool initialNoMediaReply = false;
  unsigned capturePauseCount = 0;
  auto receiverInbox = std::make_shared<preview::SessionRecovery>();
  auto senderInbox = std::make_shared<preview::SessionRecovery>();
  // 五秒基线与新 GUI 一样协商恢复能力，宿主机短暂停顿应由产品机制自行恢复。
  // recover 参数仅决定是否额外注入 120ms 停顿；200ms 密钥退休场景保留旧端兼容路径。
  if (!renewKey) {
    receiver.recovery = receiverInbox;
    receiver.onRecovery = [senderInbox, answerRecovery](const auto &message) {
      if (answerRecovery)
        senderInbox->post(message);
    };
  }
  receiver.onReady = [&](const auto &value) {
    ++readyCalls;
    ready.set_value(Offer{value});
  };
  auto worker = std::async(std::launch::async, [&] {
    preview::runSession(receiver, receiverStop, [&](const auto &event) {
      // 保留检测条件和恢复转换，实时调度受宿主机影响时能从测试输出还原失败阶段。
      if (event.state == preview::SessionState::Failed || event.state == preview::SessionState::Recovering ||
          event.state == preview::SessionState::Recovered)
        std::cout << "test_receiver reason=" << event.reason << " message=" << event.message << '\n';
      received = std::max(received, event.packets);
      recovering += event.state == preview::SessionState::Recovering;
      recovered += event.state == preview::SessionState::Recovered;
      recoveryTimedOut |= event.state == preview::SessionState::Failed && event.reason == "media-recovery-timeout";
      schedulerReported |= event.state == preview::SessionState::Recovering && event.reason == "decoder-stall";
      timelineReported |= event.state == preview::SessionState::Recovering && event.reason == "media-timeline";
      // 首次稳定播放后暂停 120ms，模拟 CPU 调度卡顿与内核批交付，避免只测试主动调用接口。
      if (recover && !paused && event.state == preview::SessionState::Connected && event.packets > 50) {
        paused = true;
        std::this_thread::sleep_for(120ms);
      }
    });
  });
  try {
    check(published.wait_for(2s) == std::future_status::ready, "Memory receiver never published its offer");
    auto offer = published.get();
    preview::SessionOptions sender;
    sender.test = true;
    sender.invitation = offer.value;
    if (!renewKey) {
      sender.recovery = senderInbox;
      sender.onRecovery = [receiverInbox](const auto &message) { receiverInbox->post(message); };
    } else if (renewKey) {
      // sender 在生成首个合成采集回调之前处理查询，必须明确回复尚无媒体，不能一直不回。
      sender.recovery = senderInbox;
      senderInbox->post({false, 1});
      sender.onRecovery = [&](const auto &message) { initialNoMediaReply = message.response && !message.hasMedia; };
    }
    if (renewKey)
      sender.mediaKeyLifetime = 200ms;
    try {
      preview::runSession(sender, senderStop, [&](const auto &event) {
        sent = std::max(sent, event.packets);
        renewing += event.state == preview::SessionState::Renewing;
        // 发送端普通 worker 的三次短暂停顿总和超过 40ms，但不能造成媒体时钟累计落后。
        // 若合成采集遗漏了媒体槽，就会触发时间线保护；接收端自己的偶发调度停顿仍由
        // 原可信恢复机制处理，不能将宿主机实时调度能力混入这个发送端媒体时钟断言。
        if (capturePauses && capturePauseCount < 3 && event.state == preview::SessionState::Connected &&
            event.packets > 50) {
          ++capturePauseCount;
          std::this_thread::sleep_for(25ms);
        }
      });
    } catch (...) {
      preview::erase(*sender.invitation);
      throw;
    }
    preview::erase(*sender.invitation);
    if (recover && !answerRecovery) {
      bool timedOut = false;
      try {
        worker.get();
      } catch (const std::exception &error) {
        timedOut = recoveryTimedOut;
      }
      check(
          timedOut && recovering == 1 && recovered == 0 && readyCalls == 1,
          "Unanswered recovery did not fail once with a bounded timeout"
      );
      std::cout << "recovery_timeout=pass offers=1 real_audio_devices=0\n";
      return;
    }
    worker.get();
  } catch (...) {
    receiverStop = true;
    if (worker.valid()) {
      try {
        worker.get();
      } catch (...) {
      }
    }
    throw;
  }
  check(
      readyCalls == 1 && received >= (renewKey ? 1 : 600) && sent >= (renewKey ? 1 : 600),
      "In-memory authenticated SRTP pair failed"
  );
  if (recover) {
    // 计数分别输出，失败时才能区分注入未发生、原因误分类和恢复未完成，保留原断言强度。
    std::cout << "worker_pause=" << paused << " scheduler_reported=" << schedulerReported
              << " recovering=" << recovering << " recovered=" << recovered << '\n';
    check(
        paused && schedulerReported && recovering >= 1 && recovered == recovering,
        "Worker stall was not classified and recovered inside the session"
    );
  }
  if (renewKey)
    check(renewing == 1, "Media key lifetime did not request a fresh session independently");
  if (renewKey)
    check(initialNoMediaReply, "Sender did not report the absence of an initial media packet");
  if (capturePauses)
    check(
        capturePauseCount == 3 && !timelineReported,
        "Synthetic capture lost its fixed media clock during short source-worker pauses"
    );
  std::cout << "memory_pair_sent=" << sent << " decoded=" << received << " onReady=" << readyCalls << '\n';
}

// 手工控制同一条 SRTP 流和可信回复的先后顺序，覆盖静默、首包及持续不可达。
enum class ReachabilityCase
{
  UdpBeforeReply,
  ReplyBeforeUdp,
  MediaBlocked,
  ConsecutiveFaults,
  InitialSilence,
  InitialResumes,
  InitialBlocked,
  InitialUnanswered,
  InitialReplyBeforeUdp,
  InitialSlowReply
};
// 不访问声卡或修改网络：媒体不可达通过保护真实 SRTP 包但不投递模拟，
// 回复先到通过诊断事件确认 Session 已处理进度后再交付首个恢复包，避免依赖线程运气。
void mediaReachability(ReachabilityCase scenario)
{
  namespace at = deskflow::audio::transport;
  const bool initial = scenario >= ReachabilityCase::InitialSilence;
  const bool blocked = scenario == ReachabilityCase::MediaBlocked || scenario == ReachabilityCase::InitialBlocked;
  const bool unanswered = scenario == ReachabilityCase::InitialUnanswered;
  const bool epochTiming =
      scenario == ReachabilityCase::InitialReplyBeforeUdp || scenario == ReachabilityCase::ConsecutiveFaults;
  const bool slowReply = scenario == ReachabilityCase::InitialSlowReply;
  auto receiver = memoryReceiver();
  // 首包无回复的正式期限现在覆盖五秒设备初始化及三秒控制等待，测试不能在五秒先结束。
  if (unanswered || slowReply)
    receiver.testDuration = 12000ms;
  auto inbox = std::make_shared<preview::SessionRecovery>();
  receiver.recovery = inbox;
  std::atomic<std::uint16_t> latest{0};
  std::atomic<unsigned> requests{0}, recovered{0}, idleEvents{0}, offers{0}, failureSnapshots{0};
  std::atomic<bool> hasMedia{false}, done{false}, graceObserved{false};
  std::atomic<bool> diagnosticsComplete{true};
  std::atomic<bool> resumeRequested{false}, resumePrepared{false};
  std::atomic<unsigned> authorizations{0};
  std::atomic<std::uint64_t> decoded{0};
  std::atomic<std::uint64_t> deferredRequest{0};
  std::atomic<bool> barrierEntered{false}, releaseBarrier{false};
  std::string failureReason;
  receiver.onRecovery = [&](const auto &request) {
    const auto number = ++requests;
    if (unanswered)
      return;
    if (slowReply || (epochTiming && number == 2)) {
      // 延迟首包回复由主测试线程模拟声卡启动；新轮次则先注入旧请求的迟到回复。
      deferredRequest = request.request;
      if (epochTiming)
        inbox->post({true, request.request - 1, latest.load(), std::uint32_t(latest.load()) * 240, true});
      return;
    }
    if (!initial && !blocked && !epochTiming && number == 2) {
      // 静默后的第二次查询启动音源；先准备一包真正加密的媒体，再按场景选择 UDP 次序。
      resumeRequested = true;
      const auto deadline = std::chrono::steady_clock::now() + 500ms;
      while (!resumePrepared && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
      check(resumePrepared, "Fixture did not prepare resumed media before progress response");
    }
    const auto sequence = latest.load();
    inbox->post({true, request.request, sequence, std::uint32_t(sequence) * 240, hasMedia.load()});
  };
  std::atomic<bool> stop{false};
  std::promise<Offer> ready;
  auto published = ready.get_future();
  receiver.onReady = [&](const auto &offer) {
    ++offers;
    ready.set_value(Offer{offer});
  };
  auto worker = std::async(std::launch::async, [&] {
    try {
      preview::runSession(receiver, stop, [&](const auto &event) {
        if (event.state == preview::SessionState::Connected)
          decoded = event.packets;
        if (event.state == preview::SessionState::Recovered)
          ++recovered;
        if (event.state == preview::SessionState::WaitingForMedia)
          ++idleEvents;
        if (event.state == preview::SessionState::Failed)
          failureReason = event.reason;
        if (event.state == preview::SessionState::Diagnostic) {
          // 日志快照必须包含归因需要的实际计数，不再只出现在被 GUI 忽略的 Connected 中。
          if (event.message.find("lost=") == std::string::npos || event.message.find("late=") == std::string::npos ||
              event.message.find("underrun=") == std::string::npos ||
              event.message.find("worker_gap_ms=") == std::string::npos)
            diagnosticsComplete = false;
          if (event.reason == "source-progress")
            graceObserved = true;
          if (event.reason == "recovery-authorized")
            ++authorizations;
          // 暂停 worker，让主线程把两个认证包同时放入内核 UDP 队列；释放后必在同轮
          // 排空中处理，不能靠随机 sleep 猜测 Session 是否观察到了中间的恢复完成。
          if (epochTiming && event.reason == (initial ? "source-progress" : "recovery-authorized") &&
              !barrierEntered.exchange(true)) {
            const auto until = std::chrono::steady_clock::now() + 2s;
            while (!releaseBarrier && !stop && std::chrono::steady_clock::now() < until)
              std::this_thread::sleep_for(1ms);
          }
          if (event.reason == "session-failure")
            ++failureSnapshots;
        }
      });
      done = true;
    } catch (...) {
      done = true;
      throw;
    }
  });
  SOCKET source = INVALID_SOCKET;
  try {
    check(published.wait_for(2s) == std::future_status::ready, "Idle fixture did not publish receiver offer");
    auto offer = published.get();
    const auto nonce = preview::randomNonce();
    source = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // receiver 的 Network 已持有 WSAStartup 引用。
    check(source != INVALID_SOCKET, "Idle fixture cannot open UDP source");
    u_long nonblocking = 1;
    check(ioctlsocket(source, FIONBIO, &nonblocking) == 0, "Idle fixture cannot set nonblocking UDP");
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(offer.value.port);
    const auto send = [&](std::span<const std::uint8_t> bytes) {
      check(
          sendto(
              source, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
              reinterpret_cast<const sockaddr *>(&target), sizeof(target)
          ) == static_cast<int>(bytes.size()),
          "Idle fixture UDP send failed"
      );
    };
    send(preview::controlPacket(offer.value, preview::ControlType::Hello, nonce));
    bool acknowledged = false;
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!acknowledged && std::chrono::steady_clock::now() < until) {
      std::array<std::uint8_t, 128> bytes{};
      const int count = recv(source, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
      preview::ControlType type{};
      preview::Nonce received{};
      if (count > 0)
        acknowledged = preview::verifyControl(offer.value, std::span(bytes).first(count), type, received) &&
                       type == preview::ControlType::Ack && received == nonce;
      std::this_thread::sleep_for(1ms);
    }
    check(acknowledged, "Idle fixture did not finish authenticated UDP handshake");
    const auto acknowledgedAt = std::chrono::steady_clock::now();
    auto material = preview::sessionMediaKey(offer.value, nonce);
    at::SrtpKey key(material);
    SecureZeroMemory(material.data(), material.size());
    at::SrtpSender sender({offer.value.ssrc, 1, 48000, 2, 240}, std::move(key));
    deskflow::audio::codec::OpusCodec codec(240);
    std::array<float, 480> pcm{};
    pcm.fill(.001f);
    std::array<std::uint8_t, at::kMaxPayloadBytes> encoded{};
    const auto count = codec.encode(pcm, encoded);
    check(count > 0, "Idle fixture Opus encoding failed");
    // 每一包都经过真实 Opus/SRTP；只有是否向 UDP 交付由测试决定。
    const auto protect = [&](std::uint16_t sequence) {
      const auto packet = sender.protect(std::span(encoded).first(count), sequence, std::uint32_t(sequence) * 240);
      check(bool(packet), "Reachability fixture SRTP protection failed");
      latest = sequence;
      hasMedia = true;
      return std::vector<std::uint8_t>(packet.packet.begin(), packet.packet.end());
    };
    const auto wait = [&](const auto &predicate, std::chrono::milliseconds timeout, const char *message) {
      const auto until = std::chrono::steady_clock::now() + timeout;
      while (!predicate() && !done && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(1ms);
      check(predicate(), message);
    };
    std::uint16_t sequence = 0;
    auto deadline = std::chrono::steady_clock::now();
    if (!initial) {
      for (; sequence < 20; ++sequence) {
        send(protect(sequence));
        deadline += 5ms;
        std::this_thread::sleep_until(deadline);
      }
    }
    if (epochTiming) {
      if (initial) {
        // 回复明确已有媒体，但首包还没有到达：引擎必须返回“授权未生效”。
        (void)protect(sequence++);
      } else {
        std::this_thread::sleep_for(120ms);
        send(protect(sequence++));
      }
      wait([&] { return barrierEntered.load(); }, 1500ms, "Recovery reply did not reach the ordering barrier");
      const auto priorAuthorizations = authorizations.load();
      send(protect(sequence++));
      sequence += 19;
      send(protect(sequence++)); // 同批中前进 20 个包周期，立即触发下一次时间线偏移。
      releaseBarrier = true;
      wait(
          [&] { return deferredRequest.load() != 0; }, 500ms, "New engine epoch did not issue a fresh recovery request"
      );
      std::this_thread::sleep_for(100ms);
      check(
          authorizations == priorAuthorizations && recovered == 0 && !done,
          "Stale reply authorized the new recovery epoch"
      );
      const auto position = latest.load();
      inbox->post({true, deferredRequest.load(), position, std::uint32_t(position) * 240, true});
      wait(
          [&] { return authorizations > priorAuthorizations; }, 500ms, "New epoch could not obtain fresh authorization"
      );
      deadline = std::chrono::steady_clock::now();
      for (const auto end = sequence + 40; sequence < end; ++sequence) {
        send(protect(sequence));
        deadline += 5ms;
        std::this_thread::sleep_until(deadline);
      }
      check(
          recovered >= 1 && offers == 1 && diagnosticsComplete && !done,
          "Authorized epoch rebuilt rather than resumed media"
      );
      stop = true;
      worker.get();
      check(decoded >= 20, "Recovered epoch did not resume actual Opus decoding");
      closesocket(source);
      std::cout << "reachability_case=" << int(scenario) << " recovery_epoch=pass authorizations=" << authorizations
                << '\n';
      return;
    }
    if (slowReply) {
      wait([&] { return deferredRequest.load() != 0; }, 1500ms, "Initial progress query was not sent promptly");
      // 模拟发送端在允许的五秒期限内接近末尾才完成初始化，尚未进入循环处理控制请求。
      // 使用真实接收 Session 和单调时钟，不能只断言一个脱离会话的超时常量。
      std::this_thread::sleep_until(acknowledgedAt + 4900ms);
      check(!done, "Initial recovery killed a sender still within its device startup budget");
      (void)protect(sequence++);
      const auto position = latest.load();
      inbox->post({true, deferredRequest.load(), position, std::uint32_t(position) * 240, true});
      wait([&] { return graceObserved.load(); }, 500ms, "Slow startup reply did not receive the bounded UDP grace");
      deadline = std::chrono::steady_clock::now();
      for (const auto end = sequence + 40; sequence < end; ++sequence) {
        send(protect(sequence));
        deadline += 5ms;
        std::this_thread::sleep_until(deadline);
      }
      check(recovered >= 1 && requests == 1 && offers == 1 && !done, "Slow startup did not resume in the same session");
      stop = true;
      worker.get();
      check(decoded >= 20, "Slow startup did not deliver decoded media after recovery");
      closesocket(source);
      std::cout << "reachability_case=" << int(scenario) << " slow_startup=pass\n";
      return;
    }
    if (blocked || unanswered) {
      if (!initial) {
        // 提前制造一次真实媒体时间线偏移，再持续产生但不交付媒体，
        // 使既有流的“三秒进度查询 + 一秒宽限”在五秒合成会话内完整结束。
        std::this_thread::sleep_for(120ms);
        send(protect(sequence++));
      }
      const auto until = std::chrono::steady_clock::now() + (unanswered ? 9000ms : 4700ms);
      while (!done && std::chrono::steady_clock::now() < until) {
        if (blocked)
          (void)protect(sequence++);
        std::this_thread::sleep_for(5ms);
      }
      check(done, "Unreachable media did not leave the session within its bounded deadline");
      if (unanswered)
        check(
            std::chrono::steady_clock::now() - acknowledgedAt >= 7800ms,
            "Initial no-reply deadline omitted the sender startup budget"
        );
      bool failed = false;
      try {
        worker.get();
      } catch (const std::exception &) {
        failed = true;
      }
      check(
          failed && failureReason == (unanswered ? "media-recovery-timeout" : "media-unreachable") &&
              failureSnapshots == 1 && offers == 1 && diagnosticsComplete,
          "Media failure did not retain its typed reason and final diagnostic snapshot"
      );
      if (blocked)
        check(graceObserved, "Missing media failed before entering the arrival grace period");
      closesocket(source);
      std::cout << "reachability_case=" << int(scenario) << " bounded_failure=" << failureReason << '\n';
      return;
    }
    if (initial) {
      // 真实初始静默超过一次进度查询周期，明确 hasMedia=false 必须保持同会话等待。
      wait([&] { return requests >= 2; }, 4500ms, "Initial silence did not query source progress twice");
      check(idleEvents == 1 && !done && offers == 1, "Initial silence was treated as an unreachable source");
    } else {
      wait([&] { return resumeRequested.load(); }, 4500ms, "Idle fixture did not query source progress twice");
      const auto packet = protect(sequence++);
      if (scenario == ReachabilityCase::UdpBeforeReply)
        send(packet);
      resumePrepared = true;
      if (scenario == ReachabilityCase::ReplyBeforeUdp) {
        wait([&] { return graceObserved.load(); }, 500ms, "Progress response did not enter bounded media grace");
        std::this_thread::sleep_for(100ms);
        check(!done, "Progress response rebuilt the session before delayed UDP could arrive");
        send(packet);
        // 只投递与最新回复位置相同的这一包，原授权不能被后续回复向前覆盖。
        wait([&] { return recovered >= 1; }, 300ms, "Progress response raised the media authorization floor");
      }
    }
    deadline = std::chrono::steady_clock::now();
    const auto endSequence = sequence + (scenario == ReachabilityCase::InitialSilence ? 0 : 40);
    for (; sequence < endSequence; ++sequence) {
      send(protect(sequence));
      deadline += 5ms;
      std::this_thread::sleep_until(deadline);
    }
    stop = true;
    worker.get();
    check(
        requests >= 2 && offers == 1 && diagnosticsComplete &&
            (scenario == ReachabilityCase::InitialSilence ? recovered == 0 : recovered >= 1),
        "Idle source was rebuilt instead of queried and resumed"
    );
    closesocket(source);
  } catch (...) {
    stop = true;
    releaseBarrier = true;
    if (source != INVALID_SOCKET)
      closesocket(source);
    if (worker.valid()) {
      try {
        worker.get();
      } catch (...) {
      }
    }
    throw;
  }
  std::cout << "reachability_case=" << int(scenario) << " same_session=pass trusted_progress_queries=" << requests
            << '\n';
}
} // namespace
int main(int argc, char **argv)
{
  try {
    if (argc == 2 && std::string(argv[1]) == "--synthetic-capture") {
      memoryPair(false, true, false, true);
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--recovery") {
      for (const auto scenario :
           {ReachabilityCase::UdpBeforeReply, ReachabilityCase::ReplyBeforeUdp, ReachabilityCase::MediaBlocked,
            ReachabilityCase::InitialSilence, ReachabilityCase::InitialResumes, ReachabilityCase::InitialBlocked})
        mediaReachability(scenario);
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--recovery-timing") {
      for (const auto scenario :
           {ReachabilityCase::InitialReplyBeforeUdp, ReachabilityCase::ConsecutiveFaults,
            ReachabilityCase::InitialSlowReply, ReachabilityCase::InitialUnanswered})
        mediaReachability(scenario);
      return 0;
    }
    validation();
    deadlineAndCallbacks();
    cooperativeWaitingStop();
    noOverlap();
    rejectsUnauthenticatedHello();
    memoryPair();
    memoryPair(true);
    memoryPair(true, false);
    memoryPair(false, true, true);
    std::cout << "Session tests passed: memory-only offers, validation, HMAC rejection, deadlines, callbacks, stop, "
                 "exclusion, SRTP\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Session tests failed: " << error.what() << '\n';
    return 1;
  }
}
