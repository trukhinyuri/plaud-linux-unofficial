// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "image.h"

#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace plaud_rec {
namespace screen {

void Image::Allocate(int w, int h) {
  width = std::max(0, w);
  height = std::max(0, h);
  rgb.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 0);
}

namespace {

constexpr uint64_t kMaxPixels = 1ull << 30;  // sanity bound on decoded rasters

bool ReadPpmNumber(const uint8_t* data, size_t size, size_t& pos, uint32_t& value) {
  for (;;) {
    while (pos < size && std::isspace(data[pos])) pos++;
    if (pos < size && data[pos] == '#') {
      while (pos < size && data[pos] != '\n') pos++;
      continue;
    }
    break;
  }
  if (pos >= size || !std::isdigit(data[pos])) return false;
  uint64_t v = 0;
  while (pos < size && std::isdigit(data[pos])) {
    v = v * 10 + static_cast<uint64_t>(data[pos] - '0');
    if (v > 0x7fffffffu) return false;
    pos++;
  }
  value = static_cast<uint32_t>(v);
  return true;
}

// libpng reports errors through longjmp; every C++ object touched between
// setjmp and a possible longjmp lives behind a pointer that is set before
// setjmp and never reassigned, so no automatic variable becomes indeterminate.
struct PngError {
  char message[256] = "libpng error";
};

void OnPngError(png_structp png, png_const_charp message) {
  auto* err = static_cast<PngError*>(png_get_error_ptr(png));
  if (err != nullptr) std::snprintf(err->message, sizeof(err->message), "%s", message);
  png_longjmp(png, 1);
}

void OnPngWarning(png_structp, png_const_charp) {}

struct DecodeState {
  PngError error;
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  Image image;
  std::vector<png_bytep> rows;
};

void ReadPngBytes(png_structp png, png_bytep out, png_size_t length) {
  auto* st = static_cast<DecodeState*>(png_get_io_ptr(png));
  if (st->size - st->pos < length) png_error(png, "truncated PNG data");
  std::memcpy(out, st->data + st->pos, length);
  st->pos += length;
}

struct EncodeState {
  PngError error;
  std::vector<uint8_t>* out = nullptr;
  std::vector<png_bytep> rows;
};

void WritePngBytes(png_structp png, png_bytep data, png_size_t length) {
  auto* st = static_cast<EncodeState*>(png_get_io_ptr(png));
  bool ok = true;
  try {
    st->out->insert(st->out->end(), data, data + length);
  } catch (const std::bad_alloc&) {
    ok = false;
  }
  if (!ok) png_error(png, "out of memory while encoding PNG");
}

void FlushPng(png_structp) {}

// Per-destination-pixel source taps along one axis.
struct AxisTaps {
  std::vector<int> first;
  std::vector<int> count;
  std::vector<int> index;
  std::vector<float> weight;
};

AxisTaps PlanAxis(int src_len, double s0, double s_len, int d_len) {
  AxisTaps t;
  t.first.resize(static_cast<size_t>(d_len));
  t.count.resize(static_cast<size_t>(d_len));
  const double ratio = s_len / d_len;
  auto clamp_index = [src_len](long k) {
    return static_cast<int>(std::min<long>(std::max<long>(k, 0), src_len - 1));
  };
  for (int i = 0; i < d_len; i++) {
    t.first[static_cast<size_t>(i)] = static_cast<int>(t.index.size());
    if (ratio > 1.0 + 1e-9) {
      const double a = std::min(std::max(s0 + i * ratio, 0.0), static_cast<double>(src_len));
      const double b = std::min(std::max(s0 + (i + 1) * ratio, 0.0), static_cast<double>(src_len));
      if (b - a <= 1e-9) {
        t.index.push_back(clamp_index(static_cast<long>(std::floor(a))));
        t.weight.push_back(1.0f);
      } else {
        const size_t start = t.index.size();
        double total = 0.0;
        const long k0 = static_cast<long>(std::floor(a));
        const long k1 = static_cast<long>(std::ceil(b)) - 1;
        for (long k = k0; k <= k1; k++) {
          const double overlap = std::min(b, static_cast<double>(k + 1)) - std::max(a, static_cast<double>(k));
          if (overlap <= 0.0) continue;
          t.index.push_back(clamp_index(k));
          t.weight.push_back(static_cast<float>(overlap));
          total += overlap;
        }
        for (size_t k = start; k < t.weight.size(); k++) t.weight[k] = static_cast<float>(t.weight[k] / total);
      }
    } else {
      const double c = s0 + (i + 0.5) * ratio - 0.5;
      const double fl = std::floor(c);
      const double f = c - fl;
      const long k0 = static_cast<long>(fl);
      if (f < 1e-6) {
        t.index.push_back(clamp_index(k0));
        t.weight.push_back(1.0f);
      } else if (f > 1.0 - 1e-6) {
        t.index.push_back(clamp_index(k0 + 1));
        t.weight.push_back(1.0f);
      } else {
        t.index.push_back(clamp_index(k0));
        t.weight.push_back(static_cast<float>(1.0 - f));
        t.index.push_back(clamp_index(k0 + 1));
        t.weight.push_back(static_cast<float>(f));
      }
    }
    t.count[static_cast<size_t>(i)] = static_cast<int>(t.index.size()) - t.first[static_cast<size_t>(i)];
  }
  return t;
}

bool IsIntegral(double v) { return std::fabs(v - std::round(v)) < 1e-9; }

}  // namespace

bool ParsePpm(const uint8_t* data, size_t size, Image& out, std::string& error) {
  if (size < 2 || data[0] != 'P' || data[1] != '6') {
    error = "not a binary PPM (P6) image";
    return false;
  }
  size_t pos = 2;
  uint32_t w = 0;
  uint32_t h = 0;
  uint32_t maxval = 0;
  if (!ReadPpmNumber(data, size, pos, w) || !ReadPpmNumber(data, size, pos, h) ||
      !ReadPpmNumber(data, size, pos, maxval) || pos >= size || !std::isspace(data[pos])) {
    error = "malformed PPM header";
    return false;
  }
  pos++;
  if (w == 0 || h == 0 || maxval == 0 || maxval > 65535 || static_cast<uint64_t>(w) * h > kMaxPixels) {
    error = "unsupported PPM dimensions or maxval";
    return false;
  }
  const size_t bytes_per_sample = maxval < 256 ? 1 : 2;
  const uint64_t samples = static_cast<uint64_t>(w) * h * 3;
  if (size - pos < samples * bytes_per_sample) {
    error = "truncated PPM pixel data";
    return false;
  }
  Image img;
  img.Allocate(static_cast<int>(w), static_cast<int>(h));
  const uint8_t* src = data + pos;
  if (bytes_per_sample == 1 && maxval == 255) {
    std::memcpy(img.rgb.data(), src, samples);
  } else {
    for (uint64_t i = 0; i < samples; i++) {
      const uint32_t v = bytes_per_sample == 1 ? src[i] : (static_cast<uint32_t>(src[2 * i]) << 8) | src[2 * i + 1];
      img.rgb[i] = static_cast<uint8_t>((v * 255u + maxval / 2) / maxval);
    }
  }
  out = std::move(img);
  return true;
}

bool DecodePng(const uint8_t* data, size_t size, Image& out, std::string& error) {
  if (size < 8 || png_sig_cmp(data, 0, 8) != 0) {
    error = "not a PNG image";
    return false;
  }
  std::unique_ptr<DecodeState> st(new DecodeState());
  st->data = data;
  st->size = size;
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &st->error, OnPngError, OnPngWarning);
  if (png == nullptr) {
    error = "png_create_read_struct failed";
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    error = "png_create_info_struct failed";
    return false;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    error = st->error.message;
    return false;
  }
  png_set_read_fn(png, st.get(), ReadPngBytes);
  png_set_user_limits(png, 65535, 65535);
  png_read_info(png, info);
  png_uint_32 w = 0;
  png_uint_32 h = 0;
  int depth = 0;
  int color = 0;
  png_get_IHDR(png, info, &w, &h, &depth, &color, nullptr, nullptr, nullptr);
  if (static_cast<uint64_t>(w) * h > kMaxPixels) png_error(png, "PNG too large");
  if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (depth == 16) png_set_strip_16(png);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
  png_set_strip_alpha(png);
  png_set_interlace_handling(png);
  png_read_update_info(png, info);
  if (png_get_rowbytes(png, info) != static_cast<size_t>(w) * 3) png_error(png, "unexpected PNG row layout");
  st->image.Allocate(static_cast<int>(w), static_cast<int>(h));
  st->rows.resize(h);
  for (png_uint_32 y = 0; y < h; y++) st->rows[y] = st->image.rgb.data() + static_cast<size_t>(y) * w * 3;
  png_read_image(png, st->rows.data());
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &info, nullptr);
  out = std::move(st->image);
  return true;
}

bool EncodePng(const Image& image, std::vector<uint8_t>& out, std::string& error) {
  out.clear();
  if (image.Empty() || image.rgb.size() != static_cast<size_t>(image.width) * image.height * 3) {
    error = "cannot encode an empty image";
    return false;
  }
  std::unique_ptr<EncodeState> st(new EncodeState());
  st->out = &out;
  st->rows.resize(static_cast<size_t>(image.height));
  for (int y = 0; y < image.height; y++) {
    st->rows[static_cast<size_t>(y)] =
        const_cast<png_bytep>(image.rgb.data() + static_cast<size_t>(y) * image.width * 3);
  }
  out.reserve(static_cast<size_t>(image.width) * image.height / 2);
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &st->error, OnPngError, OnPngWarning);
  if (png == nullptr) {
    error = "png_create_write_struct failed";
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    error = "png_create_info_struct failed";
    return false;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    error = st->error.message;
    out.clear();
    return false;
  }
  png_set_write_fn(png, st.get(), WritePngBytes, FlushPng);
  png_set_IHDR(png, info, static_cast<png_uint_32>(image.width), static_cast<png_uint_32>(image.height), 8,
               PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  // Screenshots are encoded on the caller's (JS) thread: favour speed. The
  // Up filter with zlib level 1 keeps desktop captures compact at a fraction
  // of the default level-6 adaptive-filter cost.
  png_set_compression_level(png, Z_BEST_SPEED);
  png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_UP);
  png_write_info(png, info);
  png_write_image(png, st->rows.data());
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return true;
}

void ResampleInto(const Image& src, double sx, double sy, double sw, double sh, Image& dst, int dx, int dy,
                  int dw, int dh) {
  if (src.Empty() || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
  // Clip the destination rectangle to dst; adjust the source region accordingly.
  const double kx = sw / dw;
  const double ky = sh / dh;
  if (dx < 0) {
    sx -= dx * kx;
    sw += dx * kx;
    dw += dx;
    dx = 0;
  }
  if (dy < 0) {
    sy -= dy * ky;
    sh += dy * ky;
    dh += dy;
    dy = 0;
  }
  if (dx + dw > dst.width) {
    sw -= (dx + dw - dst.width) * kx;
    dw = dst.width - dx;
  }
  if (dy + dh > dst.height) {
    sh -= (dy + dh - dst.height) * ky;
    dh = dst.height - dy;
  }
  if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

  const size_t src_stride = static_cast<size_t>(src.width) * 3;
  const size_t dst_stride = static_cast<size_t>(dst.width) * 3;
  if (std::fabs(sw - dw) < 1e-6 && std::fabs(sh - dh) < 1e-6 && IsIntegral(sx) && IsIntegral(sy)) {
    const long x0 = std::lround(sx);
    const long y0 = std::lround(sy);
    if (x0 >= 0 && y0 >= 0 && x0 + dw <= src.width && y0 + dh <= src.height) {
      for (int y = 0; y < dh; y++) {
        std::memcpy(dst.rgb.data() + static_cast<size_t>(dy + y) * dst_stride + static_cast<size_t>(dx) * 3,
                    src.rgb.data() + static_cast<size_t>(y0 + y) * src_stride + static_cast<size_t>(x0) * 3,
                    static_cast<size_t>(dw) * 3);
      }
      return;
    }
  }

  const AxisTaps tx = PlanAxis(src.width, sx, sw, dw);
  const AxisTaps ty = PlanAxis(src.height, sy, sh, dh);
  const int row_min = *std::min_element(ty.index.begin(), ty.index.end());
  const int row_max = *std::max_element(ty.index.begin(), ty.index.end());
  std::vector<std::vector<float>> rows(static_cast<size_t>(row_max - row_min + 1));
  std::vector<float> acc(static_cast<size_t>(dw) * 3);
  int freed_below = row_min;

  auto horizontal = [&](int r) -> const std::vector<float>& {
    std::vector<float>& row = rows[static_cast<size_t>(r - row_min)];
    if (!row.empty()) return row;
    row.assign(static_cast<size_t>(dw) * 3, 0.0f);
    const uint8_t* line = src.rgb.data() + static_cast<size_t>(r) * src_stride;
    for (int i = 0; i < dw; i++) {
      float s0 = 0.0f;
      float s1 = 0.0f;
      float s2 = 0.0f;
      const int f = tx.first[static_cast<size_t>(i)];
      const int n = tx.count[static_cast<size_t>(i)];
      for (int k = 0; k < n; k++) {
        const uint8_t* p = line + static_cast<size_t>(tx.index[static_cast<size_t>(f + k)]) * 3;
        const float w = tx.weight[static_cast<size_t>(f + k)];
        s0 += w * p[0];
        s1 += w * p[1];
        s2 += w * p[2];
      }
      row[static_cast<size_t>(i) * 3] = s0;
      row[static_cast<size_t>(i) * 3 + 1] = s1;
      row[static_cast<size_t>(i) * 3 + 2] = s2;
    }
    return row;
  };

  for (int j = 0; j < dh; j++) {
    const int f = ty.first[static_cast<size_t>(j)];
    const int n = ty.count[static_cast<size_t>(j)];
    // Taps are non-decreasing in j: rows below the first tap are done with.
    const int lowest = ty.index[static_cast<size_t>(f)];
    for (; freed_below < lowest; freed_below++) {
      std::vector<float>().swap(rows[static_cast<size_t>(freed_below - row_min)]);
    }
    std::fill(acc.begin(), acc.end(), 0.0f);
    for (int k = 0; k < n; k++) {
      const std::vector<float>& row = horizontal(ty.index[static_cast<size_t>(f + k)]);
      const float w = ty.weight[static_cast<size_t>(f + k)];
      for (size_t i = 0; i < acc.size(); i++) acc[i] += w * row[i];
    }
    uint8_t* out = dst.rgb.data() + static_cast<size_t>(dy + j) * dst_stride + static_cast<size_t>(dx) * 3;
    for (size_t i = 0; i < acc.size(); i++) {
      const float v = std::round(acc[i]);
      out[i] = static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    }
  }
}

Image ResizeWhole(Image&& src, int w, int h) {
  if (src.width == w && src.height == h) return std::move(src);
  Image dst;
  dst.Allocate(w, h);
  ResampleInto(src, 0.0, 0.0, src.width, src.height, dst, 0, 0, w, h);
  return dst;
}

}  // namespace screen
}  // namespace plaud_rec
