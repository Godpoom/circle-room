// Small-image decoding for analysis via Windows Imaging Component: JPEGs are
// decoded straight at 1/2..1/8 scale, so a 24 MP photo never exists in RAM.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ai {

struct SmallImage {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;  // sRGB, EXIF orientation already applied
};

struct DecodeInfo {
  int origW = 0, origH = 0;  // oriented full-size dimensions
  int64_t shotTime = 0;      // EXIF DateTimeOriginal as seconds (0 = unknown)
};

bool decodeScaled(const std::string& path, int maxDim, SmallImage& out, DecodeInfo* info, std::string& err);

// Area-average (downscale) / bilinear (upscale) resample of a source rectangle.
void resample(const SmallImage& in, float x0, float y0, float x1, float y1, int outW, int outH, SmallImage& out);
void fitLongEdge(const SmallImage& in, int maxDim, SmallImage& out);

}  // namespace ai
