// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Ogg/Opus writer producing the exact container layout of Plaud's desktop
// recorders (MacOpusEncoder / WinOpusEncoder):
//   * two logical streams: Opus (serial S) and a "PLAUD.AI" side stream (S+1);
//   * OpusHead: version 1, mono, pre-skip 0, input rate 16000, gain 0, family 0;
//   * OpusTags: vendor "libopus 1.1.4-fixed", one 68-byte comment starting
//     with "VENDOR=PALUD.AI" (sic), NUL padded, plus one trailing NUL byte;
//   * PLAUD.AI BOS packet: 300 bytes, "PLAUD.AI" followed by zeros;
//   * 48 kHz mono, 20 ms frames, hard CBR, wideband, 18 packets per page;
//     every audio page is followed by a page on S+1 carrying one 23-byte
//     zero packet with the same granule position;
//   * Finish() pads the last frame and page with silence; the final audio
//     packet and the final 23-byte tag packet carry the end-of-stream flag
//     (no zero-length packets: RFC 6716 requires packets of >= 1 byte).
// Each completed Ogg page is handed to the sink together with the audio
// duration it adds (granule delta at 48 kHz, in ms).

#pragma once

#include <ogg/ogg.h>
#include <opus.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace plaud_rec {

class PlaudOggOpusEncoder {
 public:
  static constexpr int kSampleRate = 48000;
  static constexpr int kChannels = 1;
  static constexpr int kFrameSamples = 960;      // 20 ms
  static constexpr int kPacketsPerPage = 18;     // 360 ms per audio page
  static constexpr int kMaxPacketBytes = 4000;

  using PageSink = std::function<void(const uint8_t* data, size_t size, double durationMs)>;

  PlaudOggOpusEncoder() = default;
  ~PlaudOggOpusEncoder();
  PlaudOggOpusEncoder(const PlaudOggOpusEncoder&) = delete;
  PlaudOggOpusEncoder& operator=(const PlaudOggOpusEncoder&) = delete;

  // Creates the Opus encoder and emits the three header pages.
  bool Init(int bitrate, uint32_t serial, PageSink sink, std::string* error);
  // Appends mono float samples at 48 kHz in [-1, 1]. Returns false if libopus failed.
  bool Encode(const float* pcm, size_t count, std::string* error);
  // Pads and flushes the last page with end-of-stream set on both streams. Idempotent.
  void Finish();
  bool initialized() const { return encoder_ != nullptr; }
  int64_t granule() const { return granule_; }

 private:
  bool EncodeFrame(const float* frame, bool eos, std::string* error);
  void WriteAudioPage(bool eos);
  void WritePlaudTagPage(bool eos);
  void FlushStream(ogg_stream_state* stream);

  OpusEncoder* encoder_ = nullptr;
  ogg_stream_state opusStream_{};
  ogg_stream_state plaudStream_{};
  bool streamsInit_ = false;
  bool finished_ = false;
  PageSink sink_;
  std::vector<float> frame_;
  size_t frameFill_ = 0;
  int packetsInPage_ = 0;
  int64_t opusPacketNo_ = 0;
  int64_t plaudPacketNo_ = 0;
  int64_t granule_ = 0;
  int64_t lastEmittedGranule_ = 0;
  std::vector<uint8_t> packet_;
};

}  // namespace plaud_rec
