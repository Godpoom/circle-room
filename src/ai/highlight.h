// "Recover blown highlights (AI)": areas where the photo is pure white carry no data, so
// sliders can only turn them grey. We rebuild them: colour from the bright rim of each
// area (split by semantic region), a natural brightness falloff, and fine texture from
// LaMa inpainting (luminance detail only, never its colours). The result is an RGBA
// overlay (sRGB + blend weight) that the shader mixes into the source before all edits.
#pragma once
#include <string>
#include <vector>

#include "ai/ort.h"
#include "ai/segment.h"
#include "ai/wic_decode.h"

namespace ai {

constexpr int kRecoverSize = 2048;  // overlay long edge

struct Recovery {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // sRGB colour + alpha (blend weight), EXIF-oriented
  float blown = 0;            // share of the image that was rebuilt
  int areas = 0, textured = 0;
  double ms = 0;
  bool valid() const { return w > 0 && !rgba.empty(); }
};

class Inpainter {  // LaMa, 512x512, CPU
 public:
  bool load(std::string& err);
  void release() { model_.release(); }
  bool ok() const { return model_.ok(); }
  // crop: 512x512 RGB; mask: 512*512 (1 = fill). out: 512x512 RGB.
  bool run(const SmallImage& crop, const std::vector<float>& mask, SmallImage& out, std::string& err);

 private:
  Model model_;
};

// Share of pixels with at least two channels clipped (cheap, for the UI).
float blownShare(const SmallImage& img);
bool recoverHighlights(const SmallImage& img, Inpainter* lama, const SegResult* seg, Recovery& out);

// Whole file: decode at kRecoverSize, optional regions, LaMa if available. Uses/updates the
// cache in <folder>\.photoeditor_cache\ when useCache.
bool recoverFile(const std::string& path, const SegResult* seg, bool useCache, Recovery& out, std::string& err);
bool loadRecoverCache(const std::string& imagePath, Recovery& out);

}  // namespace ai
