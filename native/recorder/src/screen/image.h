// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// 8-bit RGB raster used by the screen-capture backends, with PPM parsing,
// PNG encode/decode (libpng) and region resampling.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace plaud_rec {
namespace screen {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgb;  // width * height * 3 bytes, rows top to bottom

  bool Empty() const { return width <= 0 || height <= 0; }
  void Allocate(int w, int h);  // zero-filled (black)
};

// Binary PPM ("P6", maxval 255 or 65535), as written by grim -t ppm.
bool ParsePpm(const uint8_t* data, size_t size, Image& out, std::string& error);

// Any PNG libpng can read; alpha is dropped (screenshots are opaque).
bool DecodePng(const uint8_t* data, size_t size, Image& out, std::string& error);

// 8-bit RGB PNG.
bool EncodePng(const Image& image, std::vector<uint8_t>& out, std::string& error);

// Copies the source region [sx, sx + sw) x [sy, sy + sh), given in source
// pixel units (fractional allowed, must lie within the source), into the
// destination rectangle (dx, dy, dw, dh) of dst. Integer-aligned 1:1 regions
// are copied row by row; downscaling averages areas, upscaling is bilinear.
void ResampleInto(const Image& src, double sx, double sy, double sw, double sh, Image& dst, int dx,
                  int dy, int dw, int dh);

// Returns the whole source resampled to w x h (moves when sizes already match).
Image ResizeWhole(Image&& src, int w, int h);

}  // namespace screen
}  // namespace plaud_rec
