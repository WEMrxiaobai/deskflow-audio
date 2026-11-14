/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// 使用真实任务包装器、Session 参数校验、回环 UDP 及 Opus/SRTP；不使用 runSession 替身。
// test 模式只生成合成音频并解码丢弃，不访问声卡、本机静音、产品配置或信任记录。
#include "gui/audio/AudioSessionWorker.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void check(bool value, const std::string &message)
{
  if (!value)
    throw std::runtime_error(message);
}

// 对端直接运行真实 Session，包装器仍保持进程内仅一个任务的产品约束。
// 失败路径也先停止并等待对端退出，选项和回调引用不会先于线程失效。
struct PeerSession
{
  preview::SessionOptions options;
  std::atomic<bool> stop{false}, done{false}, connected{false};
  std::atomic<std::uint64_t> packets{0};
  std::thread thread;
  std::string failure;
  std::string lastDiagnostic, failureReason;

  ~PeerSession()
  {
    stop = true;
    if (thread.joinable())
      thread.join();
    if (options.invitation)
      preview::erase(*options.invitation);
  }

  void start()
  {
    thread = std::thread([this] {
      try {
        preview::runSession(options, stop, [this](const preview::SessionEvent &event) {
          if (event.state == preview::SessionState::Connected) {
            connected = true;
            packets = std::max(packets.load(), event.packets);
          }
          // 保留最后的有界计数快照，失败时区分契约错误、调度保护与媒体数量不足。
          if (event.state == preview::SessionState::Diagnostic)
            lastDiagnostic = event.message;
          if (event.state == preview::SessionState::Failed)
            failureReason = event.reason;
        });
      } catch (const std::exception &error) {
        failure = std::string(error.what()) + "; reason=" + failureReason + "; " + lastDiagnostic;
      }
      // 发布完成后主线程才读取 failure，避免为错误字符串增加并发读写。
      done = true;
    });
  }
};

void transfer(bool receive, bool recovery)
{
  auto workerInbox = recovery ? std::make_shared<preview::SessionRecovery>() : nullptr;
  auto peerInbox = recovery ? std::make_shared<preview::SessionRecovery>() : nullptr;
  // promise 比对端线程活得更久，异常退出时对端也不会访问已销毁的发布对象。
  std::promise<preview::Invitation> ready;
  auto offered = ready.get_future();
  PeerSession peer;
  peer.options.receive = !receive;
  peer.options.test = true;
  peer.options.recovery = peerInbox;
  if (recovery)
    peer.options.onRecovery = [workerInbox](const auto &message) { workerInbox->post(message); };
  if (!receive) {
    peer.options.onReady = [&ready](const auto &offer) { ready.set_value(offer); };
    peer.start();
    check(offered.wait_for(2s) == std::future_status::ready, "peer did not publish loopback offer");
  }

  preview::SessionOptions options;
  options.receive = receive;
  options.test = true;
  options.recovery = workerInbox;
  if (!receive)
    options.invitation = offered.get();
  // 发送端选项仅提供邀请；接收端仅提供方向。回调必须由产品包装器按真实契约正确安装。
  AudioSessionWorker worker(std::move(options), true);
  if (options.invitation)
    preview::erase(*options.invitation);
  bool connected = false, finished = false, peerStarted = !receive;
  std::uint64_t packets = 0;
  std::string lastDiagnostic, failureReason;
  const auto deadline = std::chrono::steady_clock::now() + 9s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto update = worker.takeUpdate();
    // 邮箱原本只保留八条日志；此处同样只留下最后快照，不因反复恢复积累日志。
    for (const auto &event : update.logs) {
      if (event.state == preview::SessionState::Diagnostic)
        lastDiagnostic = event.message;
      if (event.state == preview::SessionState::Failed)
        failureReason = event.reason;
    }
    if (update.offer) {
      check(receive && !peerStarted, "sender or duplicate receiver unexpectedly published an offer");
      peer.options.invitation = *update.offer;
      peer.start();
      peerStarted = true;
    }
    if (update.recovery) {
      check(recovery, "legacy session unexpectedly emitted a recovery message");
      peerInbox->post(*update.recovery);
    }
    if (update.status && update.status->state == preview::SessionState::Connected) {
      connected = true;
      packets = std::max(packets, update.status->packets);
    }
    if (update.finished) {
      check(
          update.failure.empty(),
          "worker failed: " + update.failure + "; reason=" + failureReason + "; " + lastDiagnostic
      );
      finished = true;
    }
    if (peer.done)
      check(peer.failure.empty(), "peer failed: " + peer.failure);
    if (finished && peer.done)
      break;
    std::this_thread::sleep_for(1ms);
  }
  // 完整五秒测试同时断言双方确实运行和媒体数量，不能只以“不再抛校验异常”作为通过。
  check(finished && peer.done, "worker/peer did not finish within deadline");
  check(connected && peer.connected, "worker/peer did not connect");
  check(packets >= 600 && peer.packets >= 600, "worker/peer did not transfer enough real media");
  std::cout << "role=" << (receive ? "receiver" : "sender") << " recovery=" << recovery << " worker_packets=" << packets
            << " peer_packets=" << peer.packets.load() << " real_session=1 real_opus_srtp=1 hardware=0\n";
}
} // namespace

int main(int argc, char **argv)
{
  try {
    check(argc == 3, "usage: audio-session-worker-media-tests send|receive legacy|recovery");
    const std::string role = argv[1], mode = argv[2];
    check(role == "send" || role == "receive", "invalid worker role");
    check(mode == "legacy" || mode == "recovery", "invalid recovery mode");
    transfer(role == "receive", mode == "recovery");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
