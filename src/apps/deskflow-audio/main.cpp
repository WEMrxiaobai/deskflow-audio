/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Developer-only P1 probe, not a second pairing system or a production audio service.
// Uses real UDP but binds ONLY 127.0.0.1, generates ephemeral keys in-process, and
// never accepts keys/remote addresses. Capture is never rendered (feedback guard).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <bcrypt.h>

#include "audio/engine/AudioEngine.h"
#include "audio/windows/WasapiBackend.h"
#include "audio/windows/WorkerTimer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ae = deskflow::audio::engine;
namespace aw = deskflow::audio::windows;
namespace at = deskflow::audio::transport;
using deskflow::audio::dsp::PcmFormat;

struct UdpLoop
{
  SOCKET input = INVALID_SOCKET, output = INVALID_SOCKET;
  UdpLoop()
  {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
      throw std::runtime_error("WSAStartup failed");
    input = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    output = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (input == INVALID_SOCKET || output == INVALID_SOCKET) {
      close();
      throw std::runtime_error("UDP socket failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(addr);
    u_long nonblocking = 1;
    if (bind(input, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ||
        getsockname(input, reinterpret_cast<sockaddr *>(&addr), &size) ||
        connect(output, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ||
        ioctlsocket(input, FIONBIO, &nonblocking) || ioctlsocket(output, FIONBIO, &nonblocking)) {
      close();
      throw std::runtime_error("Loopback UDP setup failed");
    }
  }
  void close() noexcept
  {
    if (input != INVALID_SOCKET)
      closesocket(input);
    if (output != INVALID_SOCKET)
      closesocket(output);
    input = output = INVALID_SOCKET;
    WSACleanup();
  }
  ~UdpLoop()
  {
    close();
  }
  bool sendPacket(std::span<const std::uint8_t> packet) noexcept
  {
    return send(output, reinterpret_cast<const char *>(packet.data()), static_cast<int>(packet.size()), 0) ==
           packet.size();
  }
};
struct DeviceContext
{
  std::atomic<ae::AudioSender *> sender{nullptr};
  std::atomic<ae::AudioReceiver *> receiver{nullptr};
  std::atomic<std::uint64_t> generation{0}, activeGeneration{0}, callbacks{0};
  std::atomic<std::uint32_t> rate{0}, channels{0}, buffer{0};
  std::atomic<std::int32_t> lastError{0};
  static void status(void *ptr, const aw::Status &s) noexcept
  {
    auto &c = *static_cast<DeviceContext *>(ptr);
    if (s.state == aw::State::Running) {
      c.rate.store(s.format.sampleRate);
      c.channels.store(s.format.channels);
      c.buffer.store(s.format.bufferFrames);
      c.generation.store(s.format.generation, std::memory_order_release);
    } else if (s.state == aw::State::Failed || s.state == aw::State::RetryPending)
      c.lastError.store(s.hresult);
  }
  static void
  capture(void *ptr, std::span<const float> pcm, const aw::DeviceFormat &f, const aw::CaptureInfo &info) noexcept
  {
    auto &c = *static_cast<DeviceContext *>(ptr);
    c.callbacks.fetch_add(1, std::memory_order_relaxed);
    if (f.generation != c.activeGeneration.load(std::memory_order_acquire))
      return;
    if (auto *sender = c.sender.load(std::memory_order_acquire))
      (void)sender->capture(pcm, info.discontinuity, ae::monotonicNowNs());
  }
  static void render(void *ptr, std::span<float> pcm, const aw::DeviceFormat &f) noexcept
  {
    auto &c = *static_cast<DeviceContext *>(ptr);
    c.callbacks.fetch_add(1, std::memory_order_relaxed);
    if (f.generation != c.activeGeneration.load(std::memory_order_acquire))
      return;
    if (auto *receiver = c.receiver.load(std::memory_order_acquire))
      (void)receiver->render(pcm);
  }
};
int main(int argc, char **argv)
{
  try {
    if (argc < 2 || std::string(argv[1]) == "--help") {
      std::cout
          << "Experimental P1 developer probe (no pairing, no remote access).\n"
             "  --self-test [seconds=2] [frame-ms=5]  Synthetic PCM -> Opus -> localhost SRTP UDP -> decode/discard\n"
             "  --capture-probe [seconds=2] [frame-ms=5]  EXPLICIT system loopback capture -> encrypted loopback -> "
             "discard\n"
             "  --render-probe [seconds=2] [frame-ms=5]  EXPLICIT quiet test tone -> encrypted loopback -> default "
             "output\n"
             "No PCM is recorded; capture is NEVER locally rendered. Not an end-to-end physical latency measurement.\n";
      return 0;
    }
    const std::string mode = argv[1];
    const bool capture = mode == "--capture-probe", render = mode == "--render-probe";
    if (!capture && !render && mode != "--self-test")
      throw std::invalid_argument("Unknown probe mode");
    const int seconds = argc > 2 ? std::stoi(argv[2]) : 2, ms = argc > 3 ? std::stoi(argv[3]) : 5;
    if (seconds < 1 || seconds > 60 || (ms != 5 && ms != 10) || argc > 4)
      throw std::invalid_argument("Require seconds 1..60 and frame-ms 5 or 10");
    DeviceContext context;
    UdpLoop udp; // Outlives sender: exception unwinding joins the sink worker before closing sockets.
    // Declared before backend: destruction always stops device callbacks before engines.
    std::unique_ptr<ae::AudioSender> sender;
    std::unique_ptr<ae::AudioReceiver> receiver;
    std::unique_ptr<aw::WasapiBackend> backend;
    PcmFormat input{48000, 2}, output{48000, 2};
    std::uint64_t deviceGeneration = 0;
    if (capture || render) {
      aw::Callbacks callbacks{&context, DeviceContext::capture, DeviceContext::render, DeviceContext::status};
      backend = std::make_unique<aw::WasapiBackend>(
          capture ? aw::Direction::LoopbackCapture : aw::Direction::Render, callbacks
      );
      if (!backend->start())
        throw std::runtime_error("Cannot start WASAPI worker");
      const auto deadline = ae::monotonicNowNs() + 5000000000ULL;
      while (!(deviceGeneration = context.generation.load(std::memory_order_acquire)) && ae::monotonicNowNs() < deadline
      )
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (!deviceGeneration)
        throw std::runtime_error(
            "No running default audio device within 5s; HRESULT=" + std::to_string(context.lastError.load())
        );
      PcmFormat device{context.rate.load(), context.channels.load()};
      if (capture)
        input = device;
      else
        output = device;
      std::cout << "device_rate=" << device.sampleRate << " channels=" << device.channels
                << " buffer_capacity_frames=" << context.buffer.load() << '\n';
    }
    std::array<std::uint8_t, 30> material{};
    std::uint32_t ssrc = 0;
    if (BCryptGenRandom(
            nullptr, material.data(), static_cast<ULONG>(material.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG
        ) < 0 ||
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&ssrc), sizeof(ssrc), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
      throw std::runtime_error("Cannot generate ephemeral probe key");
    if (!ssrc)
      ssrc = 1;
    at::SrtpKey sendKey(material), receiveKey(material);
    SecureZeroMemory(material.data(), material.size());
    at::StreamConfig config{ssrc, 1, 48000, 2, static_cast<std::uint16_t>(ms * 48)};
    receiver = std::make_unique<ae::AudioReceiver>(output, ms, config, std::move(receiveKey));
    sender = std::make_unique<ae::AudioSender>(input, ms, config, std::move(sendKey), [&](auto packet) {
      return udp.sendPacket(packet);
    });
    std::cout << "capture_adapter_lookahead_input_frames=" << sender->adaptationLatencyInputFrames()
              << " playback_adapter_lookahead_media_frames=" << receiver->adaptationLatencyMediaFrames() << '\n';
    sender->start();
    context.activeGeneration.store(deviceGeneration, std::memory_order_release);
    context.sender.store(sender.get(), std::memory_order_release);
    context.receiver.store(receiver.get(), std::memory_order_release);
    std::vector<float> tone(ms * 48 * 2), discard(render ? 0 : output.framesForDuration(ms) * output.channels);
    std::array<std::uint8_t, 1201> datagram{};
    aw::WorkerTimer timer;
    std::cout << "high_resolution_worker_timer=" << timer.highResolution() << '\n';
    const auto start = std::chrono::steady_clock::now();
    auto next = start, report = start;
    std::uint64_t frame = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
      const auto now = std::chrono::steady_clock::now();
      if (!capture && now >= next) {
        for (std::size_t i = 0; i < tone.size() / 2; ++i, ++frame)
          tone[i * 2] = tone[i * 2 + 1] =
              .02f * std::sin(static_cast<float>(frame % 48000) * 6.283185307f * 440.f / 48000.f);
        (void)sender->capture(tone, false, ae::monotonicNowNs());
        next += std::chrono::milliseconds(ms);
        if (now - next > std::chrono::milliseconds(ms))
          next = now + std::chrono::milliseconds(ms);
      }
      for (int n = 0; n < 32; ++n) {
        const int count =
            recv(udp.input, reinterpret_cast<char *>(datagram.data()), static_cast<int>(datagram.size()), 0);
        if (count == SOCKET_ERROR) {
          if (WSAGetLastError() != WSAEWOULDBLOCK)
            throw std::runtime_error("UDP receive failed");
          break;
        }
        (void)receiver->ingest(std::span(datagram.data(), count), ae::monotonicNowNs());
      }
      receiver->tick(ae::monotonicNowNs());
      if (!render && now >= report) { // Discard at a device-like cadence, no feedback output.
        (void)receiver->render(discard);
        report += std::chrono::milliseconds(ms);
        if (now - report > std::chrono::milliseconds(ms))
          report = now + std::chrono::milliseconds(ms);
      }
      if (deviceGeneration && context.generation.load(std::memory_order_acquire) != deviceGeneration)
        throw std::runtime_error("Device generation changed: probe stops; recreate engines with fresh key/format");
      if (sender->status().state == ae::EngineState::Failed)
        throw std::runtime_error("Encoder worker failed");
      if (receiver->status().state == ae::EngineState::Failed)
        throw std::runtime_error("Playback engine failed (callback quantum >10ms is unsupported by this P1 policy)");
      if (receiver->stats().requiresReanchor)
        throw std::runtime_error(
            "Media discontinuity: trusted resume/reanchor required (not integrated in this probe)"
        );
      timer.waitMicroseconds(500);
    }
    if (backend)
      backend->stop(); // Quiesce producer/consumer before engine destruction.
    context.sender.store(nullptr);
    context.receiver.store(nullptr);
    sender->stop();
    const auto s = sender->stats();
    const auto r = receiver->stats();
    std::cout << "frame_ms=" << ms << " capture_queue_capacity_packets=4 capture_queue_observed=" << s.queuedBlocks
              << " captured=" << s.capturedBlocks << " capture_dropped=" << s.captureDroppedBlocks
              << " expired=" << s.expiredBlocks << " sent=" << s.sentPackets << " sink_dropped=" << s.sinkDroppedPackets
              << " jitter_capacity_packets=8 jitter_startup_ms=5 jitter_observed=" << r.jitterPackets
              << " decoded=" << r.decodedPackets << " plc=" << r.plcPackets << " zeros=" << r.zeroPackets
              << " rejected=" << r.rejectedPackets << " decode_errors=" << r.decodeErrors
              << " playback_capacity_ms=30 playback_trim_threshold_ms=25 normalized_prewrite_target_ms=12"
              << " playback_clock_model=nominal_callback_pending playback_observed_frames=" << r.playbackFrames
              << " playback_trimmed_frames=" << r.playbackTrimmedFrames
              << " playback_rejected_frames=" << r.playbackRejectedFrames << " underrun_frames=" << r.underrunFrames
              << " correction_ppm=" << r.correctionPpm << " device_callbacks=" << context.callbacks.load()
              << " feedback_updates=" << r.feedbackUpdates << " feedback_conflicts=" << r.feedbackConflicts
              << " feedback_invalid=" << r.feedbackInvalid << " feedback_cooldown=" << r.feedbackCooldown << '\n';
    std::cout << "capture_queue_residence_mean_us="
              << (s.timingSamples ? double(s.captureResidenceNsTotal) / s.timingSamples / 1000.0 : 0)
              << " capture_queue_residence_max_us=" << s.captureResidenceNsMax / 1000.0
              << " encode_mean_us=" << (s.timingSamples ? double(s.encodeNsTotal) / s.timingSamples / 1000.0 : 0)
              << " encode_max_us=" << s.encodeNsMax / 1000.0 << " jitter_residence_mean_us="
              << (r.timingSamples ? double(r.jitterResidenceNsTotal) / r.timingSamples / 1000.0 : 0)
              << " jitter_residence_max_us=" << r.jitterResidenceNsMax / 1000.0
              << " decode_mean_us=" << (r.timingSamples ? double(r.decodeNsTotal) / r.timingSamples / 1000.0 : 0)
              << " decode_max_us=" << r.decodeNsMax / 1000.0 << " jitter_lost=" << r.jitterLostPackets
              << " jitter_late=" << r.jitterLatePackets << " requires_reanchor=" << r.requiresReanchor
              << " playback_pcm_residence_mean_us="
              << (r.playbackResidenceFrames ? double(r.playbackResidenceNsTotal) / r.playbackResidenceFrames / 1000.0
                                            : 0)
              << " playback_pcm_residence_max_us=" << r.playbackResidenceNsMax / 1000.0
              << " network_clock_correction_ppm=" << r.networkClockCorrectionPpm
              << " encoder_high_resolution_wait=" << s.highResolutionWait << '\n';
    if (!s.sentPackets || !r.decodedPackets)
      throw std::runtime_error("Probe received no decodable media (device may be idle)");
    if (!capture && (s.sentPackets < static_cast<std::uint64_t>(seconds * 1000 / ms) * 9 / 10 ||
                     r.decodedPackets * 10 < s.sentPackets * 9 || r.requiresReanchor))
      throw std::runtime_error("Probe media continuity below 90%; inspect scheduling/buffering metrics");
    std::cout << "Probe passed; this is NOT a physical end-to-end latency result.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Audio probe: " << e.what() << '\n';
    return 1;
  }
}
