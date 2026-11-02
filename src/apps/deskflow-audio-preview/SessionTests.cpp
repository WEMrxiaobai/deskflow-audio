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
#include "audio/transport/SrtpTransport.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
extern std::atomic<bool> holdPrepare, prepareEntered;
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
  receiver.handshakeTimeout = 1s;
  receiver.firstMediaTimeout = 0ms;
  rejects([&] { preview::runSession(receiver, stop); });
  receiver.firstMediaTimeout = 60001ms;
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
void cooperativePlaybackPreparation()
{
  preview::SharedPlayback playback(true);
  std::atomic<bool> ownerStop{false}, otherStop{false};
  holdPrepare = true;
  prepareEntered = false;
  auto owner = std::async(std::launch::async, [&] { return playback.prepare(ownerStop); });
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (!prepareEntered && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(1ms);
  const bool entered = prepareEntered;
  auto other = std::async(std::launch::async, [&] { return playback.prepare(otherStop); });
  std::this_thread::sleep_for(30ms);
  otherStop = true;
  const bool otherFinished = other.wait_for(500ms) == std::future_status::ready;
  ownerStop = true;
  const bool ownerFinished = owner.wait_for(500ms) == std::future_status::ready;
  holdPrepare = false;
  const auto otherResult = other.get();
  const auto ownerResult = owner.get();
  check(entered && otherFinished && ownerFinished && !ownerResult && !otherResult,
        "Playback setup did not cancel during initialization and lock contention");
  std::atomic<bool> retryStop{false};
  check(playback.prepare(retryStop).has_value(), "Playback setup could not retry after cancellation");
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
void missingFirstMedia()
{
  auto receiver = memoryReceiver();
  receiver.firstMediaTimeout = 150ms;
  receiver.controlOwnsLifetime = true;
  std::atomic<bool> stop{false};
  SOCKET socketHandle = INVALID_SOCKET;
  struct SocketGuard
  {
    SOCKET &socketHandle;
    ~SocketGuard() { if (socketHandle != INVALID_SOCKET) closesocket(socketHandle); }
  } guard{socketHandle};
  receiver.onReady = [&](const auto &offer) {
    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check(socketHandle != INVALID_SOCKET, "Cannot create test sender");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(offer.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto hello = preview::controlPacket(offer, preview::ControlType::Hello, preview::randomNonce());
    check(sendto(socketHandle, reinterpret_cast<const char *>(hello.data()), static_cast<int>(hello.size()), 0,
                 reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == static_cast<int>(hello.size()),
          "Cannot send authenticated Hello");
    // A packet from the expected endpoint is not enough: media must authenticate.
    const char invalid[] = "not authenticated media";
    check(sendto(socketHandle, invalid, sizeof(invalid), 0, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) == sizeof(invalid), "Cannot send invalid media");
  };
  bool connected = false, failed = false;
  const auto start = std::chrono::steady_clock::now();
  rejects([&] {
    preview::runSession(receiver, stop, [&](const auto &event) {
      connected |= event.state == preview::SessionState::Connected;
      failed |= event.state == preview::SessionState::Failed;
    });
  });
  check(!connected && failed && !stop, "Missing first media reported connected or failed to trigger retry");
  check(std::chrono::steady_clock::now() - start < 2s, "Missing first media did not time out promptly");
}
void authenticatedUndecodableMedia()
{
  auto receiver = memoryReceiver();
  receiver.firstMediaTimeout = 1200ms;
  receiver.controlOwnsLifetime = true;
  std::atomic<bool> stop{false};
  std::promise<Offer> ready;
  auto published = ready.get_future();
  receiver.onReady = [&](const auto &value) { ready.set_value(Offer{value}); };
  std::string failure, report;
  bool connected = false;
  auto worker = std::async(std::launch::async, [&] {
    try {
      preview::runSession(receiver, stop, [&](const auto &event) {
        connected |= event.state == preview::SessionState::Connected;
        if (event.message.find("received=") == 0)
          report = event.message;
      });
    } catch (const std::exception &error) {
      failure = error.what();
    }
  });
  SOCKET socketHandle = INVALID_SOCKET;
  try {
    check(published.wait_for(2s) == std::future_status::ready, "Receiver did not publish an offer");
    auto offer = published.get();
    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check(socketHandle != INVALID_SOCKET, "Cannot create synthetic sender");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(offer.value.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto nonce = preview::randomNonce();
    const auto hello = preview::controlPacket(offer.value, preview::ControlType::Hello, nonce);
    check(sendto(socketHandle, reinterpret_cast<const char *>(hello.data()), static_cast<int>(hello.size()), 0,
                 reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == static_cast<int>(hello.size()),
          "Cannot send Hello");
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socketHandle, &readable);
    timeval timeout{2, 0};
    check(select(0, &readable, nullptr, nullptr, &timeout) == 1, "Receiver did not acknowledge Hello");
    std::array<char, 128> ack{};
    check(recv(socketHandle, ack.data(), static_cast<int>(ack.size()), 0) > 0, "Cannot read ACK");
    auto material = preview::sessionMediaKey(offer.value, nonce);
    deskflow::audio::transport::SrtpKey key(material);
    deskflow::audio::transport::SrtpKey verificationKey(material);
    SecureZeroMemory(material.data(), material.size());
    deskflow::audio::transport::SrtpSender sender({offer.value.ssrc, 1, 48000, 2, 240}, std::move(key));
    deskflow::audio::transport::SrtpReceiver verifier({offer.value.ssrc, 1, 48000, 2, 240}, std::move(verificationKey));
    // Opus code 3 requires a second byte; this authenticated one-byte payload is invalid.
    const std::array<std::uint8_t, 1> invalid{0x03};
    std::uint16_t sequence = 0;
    const auto limit = std::chrono::steady_clock::now() + 3s;
    auto nextPacket = std::chrono::steady_clock::now();
    while (worker.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < limit) {
      while (std::chrono::steady_clock::now() < nextPacket)
        std::this_thread::yield();
      const auto packet = sender.protect(invalid, sequence, std::uint32_t(sequence) * 240);
      check(bool(packet), "Cannot authenticate synthetic invalid Opus");
      check(bool(verifier.unprotect(packet.packet)), "Synthetic packet did not pass SRTP authentication");
      const auto sent = sendto(socketHandle, reinterpret_cast<const char *>(packet.packet.data()),
                               static_cast<int>(packet.packet.size()), 0,
                               reinterpret_cast<const sockaddr *>(&address), sizeof(address));
      if (sent != static_cast<int>(packet.packet.size()) && worker.wait_for(20ms) == std::future_status::ready)
        break; // Receiver closed its socket as the timeout fired.
      check(sent == static_cast<int>(packet.packet.size()), "Cannot send invalid Opus");
      ++sequence;
      nextPacket += 5ms;
    }
    const bool finished = worker.wait_for(0ms) == std::future_status::ready;
    if (!finished)
      stop = true;
    worker.get();
    closesocket(socketHandle);
    socketHandle = INVALID_SOCKET;
    if (!finished || failure.find("首个可解码") == std::string::npos || connected || sequence == 0)
      std::cerr << "Undecodable stream: finished=" << finished << " connected=" << connected
                << " packets=" << sequence << " failure=" << failure << '\n';
    check(finished && failure.find("首个可解码") != std::string::npos && !connected && sequence > 0,
          "Undecodable authenticated stream did not hit the first-decode deadline");
    check(report.find("received=") == 0 && report.find("received=0") != 0 &&
              report.find("decoded=0") != std::string::npos,
          "Receiver did not authenticate synthetic packets without decoding them");
  } catch (...) {
    stop = true;
    if (socketHandle != INVALID_SOCKET)
      closesocket(socketHandle);
    if (worker.valid())
      worker.get();
    throw;
  }
}
void memoryPair(bool ipv6 = false)
{
  auto receiver = memoryReceiver();
  receiver.expectedSenderAddress = ipv6 ? "::1" : "127.0.0.1";
  std::atomic<bool> receiverStop{false}, senderStop{false};
  std::promise<Offer> ready;
  auto published = ready.get_future();
  std::uint64_t received = 0, sent = 0;
  bool prematureConnected = false;
  int readyCalls = 0;
  receiver.onReady = [&](const auto &value) {
    ++readyCalls;
    Offer offer{value};
    if (ipv6)
      offer.value.addresses = {"::1"};
    ready.set_value(offer);
  };
  auto worker = std::async(std::launch::async, [&] {
    preview::runSession(receiver, receiverStop, [&](const auto &event) {
      prematureConnected |= event.state == preview::SessionState::Connected && event.packets == 0;
      received = std::max(received, event.packets);
    });
  });
  try {
    check(published.wait_for(2s) == std::future_status::ready, "Memory receiver never published its offer");
    auto offer = published.get();
    preview::SessionOptions sender;
    sender.test = true;
    sender.invitation = offer.value;
    try {
      preview::runSession(sender, senderStop, [&](const auto &event) { sent = std::max(sent, event.packets); });
    } catch (...) {
      preview::erase(*sender.invitation);
      throw;
    }
    preview::erase(*sender.invitation);
    worker.get();
  } catch (...) {
    receiverStop = true;
    if (worker.valid()) {
      try {
        worker.get();
      } catch (const std::exception &error) {
        std::cerr << "Receiver error: " << error.what() << '\n';
      }
    }
    throw;
  }
  check(!prematureConnected, "Receiver reported connected before decoding media");
  check(readyCalls == 1 && received >= 600 && sent >= 600, "In-memory authenticated SRTP pair failed");
  std::cout << "memory_pair_sent=" << sent << " decoded=" << received << " onReady=" << readyCalls << '\n';
}
} // namespace
int main()
{
  try {
    validation();
    deadlineAndCallbacks();
    cooperativeWaitingStop();
    cooperativePlaybackPreparation();
    noOverlap();
    rejectsUnauthenticatedHello();
    missingFirstMedia();
    authenticatedUndecodableMedia();
    memoryPair();
    memoryPair(true);
    std::cout << "Session tests passed: memory-only offers, validation, HMAC rejection, deadlines, callbacks, stop, "
                 "exclusion, SRTP\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Session tests failed: " << error.what() << '\n';
    return 1;
  }
}
