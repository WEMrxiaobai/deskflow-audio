/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "OpusCodec.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace deskflow::audio::codec {
OpusCodec::OpusCodec(std::uint16_t frameSamples, int bitrate) : m_frames(frameSamples)
{
  if ((m_frames != 240 && m_frames != 480) || bitrate < 32000 || bitrate > 256000)
    throw std::invalid_argument("Unsupported Opus prototype format");
  int error = OPUS_OK;
  m_encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
  if (!m_encoder || error != OPUS_OK)
    throw std::runtime_error("Cannot create Opus encoder");
  m_decoder = opus_decoder_create(48000, 2, &error);
  if (!m_decoder || error != OPUS_OK) {
    opus_encoder_destroy(m_encoder);
    m_encoder = nullptr;
    throw std::runtime_error("Cannot create Opus decoder");
  }
  const bool configured = opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate)) == OPUS_OK &&
                          opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1)) == OPUS_OK &&
                          opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5)) == OPUS_OK &&
                          opus_encoder_ctl(m_encoder, OPUS_SET_FORCE_CHANNELS(2)) == OPUS_OK &&
                          opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0)) == OPUS_OK &&
                          opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(0)) == OPUS_OK &&
                          opus_encoder_ctl(m_encoder, OPUS_GET_LOOKAHEAD(&m_lookahead)) == OPUS_OK;
  if (!configured) {
    opus_encoder_destroy(m_encoder);
    opus_decoder_destroy(m_decoder);
    m_encoder = nullptr;
    m_decoder = nullptr;
    throw std::runtime_error("Cannot configure low-delay Opus");
  }
}
OpusCodec::~OpusCodec()
{
  opus_encoder_destroy(m_encoder);
  opus_decoder_destroy(m_decoder);
}
int OpusCodec::encode(std::span<const float> pcm, std::span<std::uint8_t> packet) noexcept
{
  if (pcm.size() != std::size_t(m_frames) * 2 || packet.empty() || packet.size() > 1178 ||
      !std::all_of(pcm.begin(), pcm.end(), [](float sample) { return std::isfinite(sample); }))
    return OPUS_BAD_ARG;
  return opus_encode_float(m_encoder, pcm.data(), m_frames, packet.data(), static_cast<opus_int32>(packet.size()));
}
int OpusCodec::decode(std::span<const std::uint8_t> packet, std::span<float> pcm) noexcept
{
  if (pcm.size() < std::size_t(m_frames) * 2 || packet.size() > 1178)
    return OPUS_BAD_ARG;
  // Reject other durations/channels before mutating decoder state. FEC is intentionally
  // not used with restricted-lowdelay CELT; there is no extra packet wait.
  if (!packet.empty() &&
      (opus_packet_get_nb_samples(packet.data(), static_cast<opus_int32>(packet.size()), 48000) != m_frames ||
       opus_packet_get_nb_channels(packet.data()) != 2))
    return OPUS_INVALID_PACKET;
  return opus_decode_float(
      m_decoder, packet.empty() ? nullptr : packet.data(), static_cast<opus_int32>(packet.size()), pcm.data(), m_frames,
      0
  );
}
void OpusCodec::resetEncoder() noexcept
{
  (void)opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);
}
void OpusCodec::resetDecoder() noexcept
{
  (void)opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
}
} // namespace deskflow::audio::codec
