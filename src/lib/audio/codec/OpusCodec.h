/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <cstdint>
#include <opus.h>
#include <span>

namespace deskflow::audio::codec {

// Worker-thread only. 48kHz interleaved stereo; exactly 5ms or 10ms. No VAD/DTX,
// FEC or hidden lookahead buffering. PLC is requested explicitly with an empty packet.
class OpusCodec final
{
public:
  explicit OpusCodec(std::uint16_t frameSamples, int bitrate = 160000);
  ~OpusCodec();
  OpusCodec(const OpusCodec &) = delete;
  OpusCodec &operator=(const OpusCodec &) = delete;
  int encode(std::span<const float> pcm, std::span<std::uint8_t> packet) noexcept;
  int decode(std::span<const std::uint8_t> packet, std::span<float> pcm) noexcept;
  void resetEncoder() noexcept;
  void resetDecoder() noexcept;
  [[nodiscard]] int lookaheadFrames() const noexcept
  {
    return m_lookahead;
  }
  [[nodiscard]] std::uint16_t frameSamples() const noexcept
  {
    return m_frames;
  }

private:
  OpusEncoder *m_encoder = nullptr;
  OpusDecoder *m_decoder = nullptr;
  std::uint16_t m_frames;
  int m_lookahead = 0;
};
} // namespace deskflow::audio::codec
