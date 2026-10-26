// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "audio/plaud_ogg_opus.h"

#include <algorithm>
#include <cstring>

namespace plaud_rec {

namespace {

constexpr size_t kPlaudBosBytes = 300;
constexpr size_t kPlaudTagBytes = 23;
constexpr char kVendor[] = "libopus 1.1.4-fixed";
constexpr char kComment[] = "VENDOR=PALUD.AI";
constexpr uint32_t kCommentFieldBytes = 68;

void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

std::vector<uint8_t> BuildOpusHead() {
  // RFC 7845 identification header; pre-skip 0 and "input sample rate"
  // 16000 are what the official encoders write.
  std::vector<uint8_t> h = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0, 0};
  PutLe32(h, 16000);
  h.push_back(0);  // output gain (LE16)
  h.push_back(0);
  h.push_back(0);  // channel mapping family
  return h;
}

std::vector<uint8_t> BuildOpusTags() {
  std::vector<uint8_t> t = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
  const uint32_t vendorLen = sizeof(kVendor) - 1;
  PutLe32(t, vendorLen);
  t.insert(t.end(), kVendor, kVendor + vendorLen);
  PutLe32(t, 1);
  PutLe32(t, kCommentFieldBytes);
  const size_t commentStart = t.size();
  t.insert(t.end(), kComment, kComment + sizeof(kComment) - 1);
  t.resize(commentStart + kCommentFieldBytes, 0);
  t.push_back(0);  // trailing byte present in the official files (108 bytes total)
  return t;
}

}  // namespace

PlaudOggOpusEncoder::~PlaudOggOpusEncoder() {
  if (encoder_ != nullptr) opus_encoder_destroy(encoder_);
  if (streamsInit_) {
    ogg_stream_clear(&opusStream_);
    ogg_stream_clear(&plaudStream_);
  }
}

bool PlaudOggOpusEncoder::Init(int bitrate, uint32_t serial, PageSink sink, std::string* error) {
  int err = OPUS_OK;
  encoder_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_AUDIO, &err);
  if (encoder_ == nullptr || err != OPUS_OK) {
    if (error) *error = std::string("opus_encoder_create failed: ") + opus_strerror(err);
    encoder_ = nullptr;
    return false;
  }
  if (opus_encoder_ctl(encoder_, OPUS_SET_VBR(0)) != OPUS_OK ||
      opus_encoder_ctl(encoder_, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)) != OPUS_OK ||
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) {
    if (error) *error = "opus_encoder_ctl failed (VBR/BANDWIDTH/BITRATE)";
    opus_encoder_destroy(encoder_);
    encoder_ = nullptr;
    return false;
  }
  serial &= 0x7ffffffe;  // keep serial+1 representable
  if (ogg_stream_init(&opusStream_, static_cast<int>(serial)) != 0) {
    if (error) *error = "Failed to initialize OGG stream";
    return false;
  }
  if (ogg_stream_init(&plaudStream_, static_cast<int>(serial + 1)) != 0) {
    ogg_stream_clear(&opusStream_);
    if (error) *error = "Failed to initialize PLAUD.AI stream";
    return false;
  }
  streamsInit_ = true;
  sink_ = std::move(sink);
  frame_.assign(kFrameSamples, 0.f);
  packet_.resize(kMaxPacketBytes);

  std::vector<uint8_t> head = BuildOpusHead();
  ogg_packet op{};
  op.packet = head.data();
  op.bytes = static_cast<long>(head.size());
  op.b_o_s = 1;
  op.granulepos = 0;
  op.packetno = opusPacketNo_++;
  ogg_stream_packetin(&opusStream_, &op);
  FlushStream(&opusStream_);

  std::vector<uint8_t> plaudBos(kPlaudBosBytes, 0);
  std::memcpy(plaudBos.data(), "PLAUD.AI", 8);
  op = ogg_packet{};
  op.packet = plaudBos.data();
  op.bytes = static_cast<long>(plaudBos.size());
  op.b_o_s = 1;
  op.granulepos = 0;
  op.packetno = plaudPacketNo_++;
  ogg_stream_packetin(&plaudStream_, &op);
  FlushStream(&plaudStream_);

  std::vector<uint8_t> tags = BuildOpusTags();
  op = ogg_packet{};
  op.packet = tags.data();
  op.bytes = static_cast<long>(tags.size());
  op.granulepos = 0;
  op.packetno = opusPacketNo_++;
  ogg_stream_packetin(&opusStream_, &op);
  FlushStream(&opusStream_);
  return true;
}

bool PlaudOggOpusEncoder::Encode(const float* pcm, size_t count, std::string* error) {
  if (encoder_ == nullptr || finished_) return false;
  bool ok = true;
  while (count > 0) {
    const size_t take = std::min(count, static_cast<size_t>(kFrameSamples) - frameFill_);
    std::memcpy(frame_.data() + frameFill_, pcm, take * sizeof(float));
    frameFill_ += take;
    pcm += take;
    count -= take;
    if (frameFill_ == static_cast<size_t>(kFrameSamples)) {
      frameFill_ = 0;
      if (!EncodeFrame(frame_.data(), false, error)) ok = false;
    }
  }
  return ok;
}

bool PlaudOggOpusEncoder::EncodeFrame(const float* frame, bool eos, std::string* error) {
  const opus_int32 bytes =
      opus_encode_float(encoder_, frame, kFrameSamples, packet_.data(), static_cast<opus_int32>(packet_.size()));
  if (bytes < 0) {
    if (error) *error = std::string("opus_encode_float failed: ") + opus_strerror(bytes);
    return false;
  }
  granule_ += kFrameSamples;
  ogg_packet op{};
  op.packet = packet_.data();
  op.bytes = bytes;
  op.granulepos = granule_;
  op.packetno = opusPacketNo_++;
  op.e_o_s = eos ? 1 : 0;
  ogg_stream_packetin(&opusStream_, &op);
  if (++packetsInPage_ == kPacketsPerPage || eos) WriteAudioPage(eos);
  return true;
}

void PlaudOggOpusEncoder::WriteAudioPage(bool eos) {
  packetsInPage_ = 0;
  FlushStream(&opusStream_);
  WritePlaudTagPage(eos);
}

void PlaudOggOpusEncoder::WritePlaudTagPage(bool eos) {
  uint8_t zeros[kPlaudTagBytes] = {};
  ogg_packet op{};
  op.packet = zeros;
  op.bytes = kPlaudTagBytes;
  op.granulepos = granule_;
  op.packetno = plaudPacketNo_++;
  op.e_o_s = eos ? 1 : 0;
  ogg_stream_packetin(&plaudStream_, &op);
  FlushStream(&plaudStream_);
}

void PlaudOggOpusEncoder::FlushStream(ogg_stream_state* stream) {
  ogg_page og;
  while (ogg_stream_flush(stream, &og) != 0) {
    double durationMs = 0;
    if (stream == &opusStream_) {
      const int64_t pageGranule = ogg_page_granulepos(&og);
      if (pageGranule > lastEmittedGranule_) {
        durationMs = static_cast<double>(pageGranule - lastEmittedGranule_) * 1000.0 / kSampleRate;
        lastEmittedGranule_ = pageGranule;
      }
    }
    if (sink_) {
      std::vector<uint8_t> page(static_cast<size_t>(og.header_len + og.body_len));
      std::memcpy(page.data(), og.header, static_cast<size_t>(og.header_len));
      std::memcpy(page.data() + og.header_len, og.body, static_cast<size_t>(og.body_len));
      sink_(page.data(), page.size(), durationMs);
    }
  }
}

void PlaudOggOpusEncoder::Finish() {
  if (encoder_ == nullptr || finished_) return;
  finished_ = true;
  // Complete the partial frame with silence, then pad the open page (or one
  // whole page at an exact page boundary); the page's last packet ends both
  // streams.
  if (frameFill_ > 0) {
    std::fill(frame_.begin() + static_cast<std::ptrdiff_t>(frameFill_), frame_.end(), 0.f);
    frameFill_ = 0;
  } else {
    std::fill(frame_.begin(), frame_.end(), 0.f);
  }
  for (;;) {
    const bool last = packetsInPage_ == kPacketsPerPage - 1;
    if (!EncodeFrame(frame_.data(), last, nullptr)) {
      // libopus refused the frame: close the partial page as is.
      if (packetsInPage_ > 0) WriteAudioPage(true);
      return;
    }
    if (last) return;
    std::fill(frame_.begin(), frame_.end(), 0.f);
  }
}

}  // namespace plaud_rec
