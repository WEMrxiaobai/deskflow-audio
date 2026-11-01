/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "audio/codec/OpusCodec.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using deskflow::audio::codec::OpusCodec;
static void check(bool ok, const char *message)
{
  if (!ok)
    throw std::runtime_error(message);
}
int main()
{
  try {
    for (const auto frames : {240, 480}) {
      OpusCodec codec(static_cast<std::uint16_t>(frames));
      std::vector<float> pcm(frames * 2), decoded(frames * 2);
      std::array<std::uint8_t, 1178> packet{};
      for (int i = 0; i < frames; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] = .05f * std::sin(float(i) * .1f);
      const int bytes = codec.encode(pcm, packet);
      check(bytes > 0 && bytes <= 1178, "bounded encode");
      check(codec.decode(std::span(packet.data(), bytes), decoded) == frames, "decode exact duration");
      check(codec.lookaheadFrames() > 0 && codec.lookaheadFrames() < frames, "query actual codec lookahead");
      check(codec.decode({}, decoded) == frames, "explicit PLC");
      check(codec.encode(std::span(pcm.data(), pcm.size() - 1), packet) == OPUS_BAD_ARG, "reject partial frame");
      pcm[0] = std::numeric_limits<float>::quiet_NaN();
      check(codec.encode(pcm, packet) == OPUS_BAD_ARG, "reject nonfinite capture");
      codec.resetDecoder();
      codec.resetEncoder();
      std::cout << "frame_ms=" << frames / 48 << " opus_lookahead_frames=" << codec.lookaheadFrames() << '\n';
    }
    bool rejected = false;
    try {
      OpusCodec bad(120);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    check(rejected, "reject unnegotiated duration");
    OpusCodec five(240), ten(480);
    std::array<float, 960> pcm{};
    std::array<std::uint8_t, 1178> packet{};
    int bytes = ten.encode(pcm, packet);
    check(five.decode(std::span(packet.data(), bytes), pcm) == OPUS_INVALID_PACKET, "reject old-format packet");
    std::cout << "Codec tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
