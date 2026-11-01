/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// MSVC Debug CRT hooks cover C malloc/realloc/free as well as C++ new/delete.
// Scope: application callback bodies and DSP entry points, not OS/driver callback internals.
#if defined(_MSC_VER) && defined(_DEBUG)
#include "audio/codec/OpusCodec.h"
#include "audio/dsp/BandLimitedAdapter.h"
#include "audio/engine/AudioEngine.h"
#include <array>
#include <crtdbg.h>
#include <iostream>
#include <malloc.h>
#include <stdexcept>
#include <vector>

namespace {
using namespace deskflow::audio;
using namespace deskflow::audio::engine;
thread_local bool observing = false;
thread_local std::size_t allocations = 0, reallocations = 0, deallocations = 0;
int __cdecl allocationHook(int action, void *, std::size_t, int, long, const unsigned char *, int)
{
  if (observing) {
    if (action == _HOOK_ALLOC)
      ++allocations;
    if (action == _HOOK_REALLOC)
      ++reallocations;
    if (action == _HOOK_FREE)
      ++deallocations;
  }
  return 1;
}
void require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
class Observe
{
public:
  Observe()
  {
    allocations = reallocations = deallocations = 0;
    observing = true;
  }
  ~Observe()
  {
    observing = false;
  }
};
class HookLifetime
{
public:
  HookLifetime() : previous(_CrtSetAllocHook(allocationHook))
  {
  }
  ~HookLifetime()
  {
    _CrtSetAllocHook(previous);
  }

private:
  _CRT_ALLOC_HOOK previous;
};
void requireNoHeap(const char *message)
{
  require(allocations == 0 && reallocations == 0 && deallocations == 0, message);
}
void verifyHook()
{
  {
    Observe guard;
    void *pointer = _malloc_dbg(16, _NORMAL_BLOCK, __FILE__, __LINE__);
    pointer = _realloc_dbg(pointer, 32, _NORMAL_BLOCK, __FILE__, __LINE__);
    _free_dbg(pointer, _NORMAL_BLOCK);
    auto *cpp = new int[16];
    delete[] cpp;
    auto *aligned = _aligned_malloc_dbg(64, 64, __FILE__, __LINE__);
    _aligned_free_dbg(aligned);
  }
  require(
      allocations >= 3 && reallocations >= 1 && deallocations >= 3,
      "CRT hook self-test failed; zero-allocation evidence would be invalid"
  );
}
SrtpKey publicTestKey()
{
  std::array<std::uint8_t, transport::kMasterKeySaltBytes> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>(i + 17);
  return SrtpKey(bytes);
}
void dspCallbacks()
{
  for (const auto rate : {8000U, 32000U, 44100U, 48000U, 96000U, 192000U}) {
    dsp::BandLimitedAdapter adapter({rate, 2}, {48000, 2});
    std::vector<float> input((static_cast<std::size_t>(rate) / 100 + 1) * 2, .00001f);
    std::array<float, 1024> output{};
    bool valid = true;
    {
      Observe guard;
      for (std::size_t iteration = 0; iteration < 100; ++iteration) {
        valid = adapter.setRateCorrectionPpm(static_cast<double>(iteration) - 50) && valid;
        auto remaining = std::span<const float>(input);
        for (;;) {
          const auto progress = adapter.process(remaining, output);
          remaining = remaining.subspan(progress.inputFrames * 2);
          if (progress.inputFrames == 0 && progress.outputFrames == 0)
            break;
        }
        if (iteration % 10 == 0)
          adapter.reset();
      }
    }
    requireNoHeap("FIR process/setRateCorrectionPpm/reset used CRT heap");
    require(valid, "FIR rejected rate correction in realtime test");
  }
}
void senderCallbacks()
{
  for (const auto rate : {44100U, 48000U, 96000U, 192000U}) {
    const StreamConfig config{0x12000000U + rate, 1, 48000, 2, 240};
    AudioSender sender({rate, 2}, 5, config, publicTestKey(), [](auto) { return true; });
    std::vector<float> pcm((static_cast<std::size_t>(rate) / 100 + 1) * 2, .00001f);
    sender.start();
    bool valid = true;
    {
      Observe guard;
      for (unsigned i = 0; i < 100; ++i) {
        valid = sender.capture(pcm, i % 10 == 0, monotonicNowNs()) && valid;
      }
    }
    requireNoHeap("AudioSender capture callback allocated/reallocated/freed heap memory");
    require(valid, "Capture input rejected in realtime test");
    sender.stop();
  }
}
void receiverCallbacks()
{
  const StreamConfig config{0x87654321, 1, 48000, 2, 240};
  codec::OpusCodec opus(240);
  transport::SrtpSender sender(config, publicTestKey());
  AudioReceiver a({44100, 2}, 5, config, publicTestKey());
  AudioReceiver b({44100, 2}, 5, config, publicTestKey());
  std::array<AudioReceiver *, 2> sources{&a, &b};
  std::array<float, 480> pcm{};
  pcm.fill(.00001f);
  std::array<std::uint8_t, transport::kMaxPayloadBytes> encoded{};
  std::array<float, 442> output{};
  const auto start = monotonicNowNs();
  for (std::uint16_t i = 0; i < 100; ++i) {
    const auto count = opus.encode(pcm, encoded);
    require(count > 0, "Realtime fixture encode failed");
    const auto protectedPacket = sender.protect(std::span<const std::uint8_t>(encoded.data(), count), i, i * 240U);
    require(bool(protectedPacket), "Realtime fixture protect failed");
    const auto arrival = start + std::uint64_t(i) * 5000000;
    require(
        a.ingest(protectedPacket.packet, arrival) && b.ingest(protectedPacket.packet, arrival),
        "Realtime fixture authenticated ingest failed"
    );
    a.tick(arrival + 5000000);
    b.tick(arrival + 5000000);
    bool valid = true;
    {
      Observe guard;
      a.setVolume(.4f);
      b.setMuted(i % 10 == 0);
      valid = AudioReceiver::mix(sources, output);
    }
    requireNoHeap("Two-source playback mix/controls allocated/reallocated/freed heap memory");
    require(valid, "Realtime fixture mix rejected");
  }
  a.reanchor(100, 24000, start + 500000000);
  {
    Observe guard;
    (void)a.render(output); // Includes pending reader flush acknowledgement.
    for (unsigned i = 0; i < 20; ++i)
      (void)a.render(output); // Exact-zero/underrun path.
  }
  requireNoHeap("Playback reanchor flush/underrun allocated/reallocated/freed heap memory");
}
} // namespace
int main()
{
  try {
    HookLifetime hooks;
    verifyHook();
    dspCallbacks();
    senderCallbacks();
    receiverCallbacks();
    std::cout << "MSVC Debug CRT realtime heap checks passed: C/C++ allocation, reallocation and free = 0\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Realtime heap checks failed: " << error.what() << '\n';
    return 1;
  }
}
#else
#include <iostream>
int main()
{
  std::cout << "Skipped: requires MSVC Debug CRT; Release/non-MSVC is not evidence of zero C heap activity\n";
  return 77;
}
#endif
