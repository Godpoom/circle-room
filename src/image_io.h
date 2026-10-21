#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Decoded image, kept in RAM only until it is uploaded to the GPU.
struct ImageData {
  int w = 0, h = 0;
  bool is16 = false;               // true: px16 holds *linear* 16-bit RGB
  std::vector<uint8_t> px8;        // sRGB-encoded RGB
  std::vector<uint16_t> px16;
  int orientation = 1;             // EXIF orientation 1..8
  void release() {
    std::vector<uint8_t>().swap(px8);
    std::vector<uint16_t>().swap(px16);
  }
};

// maxDim: images larger than this (GPU texture limit) are box-downscaled.
bool loadImage(const std::string& path, int maxDim, ImageData& out, std::string& err);
// In-memory JPEG helpers (thumbnails).
bool encodeJpegMem(const uint8_t* rgb, int w, int h, int quality, std::vector<uint8_t>& out);
bool decodeRgbMem(const uint8_t* data, size_t size, std::vector<uint8_t>& rgb, int& w, int& h);
// RGBA PNG (AI overlays / caches).
bool writePngRgba(const std::string& path, const uint8_t* rgba, int w, int h, std::string& err);
bool loadRgbaFile(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h);
bool writeImage(const std::string& path, const uint8_t* rgb, int w, int h, bool png, int quality, std::string& err);
