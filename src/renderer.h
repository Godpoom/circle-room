#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "image_io.h"
#include "params.h"

struct M3 {
  float m[9];  // row-major affine
};

// Maps output-image coordinates (0..1, y down) to the source texture.
struct Geometry {
  M3 outToSrc;
  M3 outToFrame;
  M3 outToOriented;    // output -> EXIF-oriented image coords (where AI masks live)
  int frameW, frameH;  // after EXIF orientation + rotate90 (before crop)
  int outW, outH;      // crop size in pixels (or the frame when fullFrame)
};

M3 invertM3(const M3& m);
void applyM3(const M3& m, float x, float y, float& ox, float& oy);
// Photo layer placement: layer uv (0..1) -> base photo's oriented uv. Wo/Ho = oriented base size.
M3 layerToOriented(const ImageLayer& l, int Wo, int Ho, int lw, int lh);

Geometry computeGeometry(const EditParams& p, int srcW, int srcH, int orient, bool fullFrame);
void frameDims(const EditParams& p, int srcW, int srcH, int orient, int& w, int& h);
bool cropFits(const EditParams& p, int frameW, int frameH);
void constrainCrop(EditParams& p, int frameW, int frameH);

class Renderer {
 public:
  bool init(std::string& err);
  void shutdown();
  // Uploads to the GPU and frees the CPU copy in img.
  bool upload(ImageData& img, std::string& err);
  bool hasImage() const { return srcTex_ != 0; }
  int srcW() const { return srcW_; }
  int srcH() const { return srcH_; }
  int orientation() const { return orient_; }
  int maxTextureSize() const { return maxTex_; }

  // Renders output region [o0,o1] into a w*h texture. zoom = screen px per output px.
  // Returns the GL texture; (uMax, vMax) is the used part of it.
  unsigned renderView(int slot, const EditParams& p, bool fullFrame, float o0x, float o0y, float o1x, float o1y,
                      int w, int h, float zoom, bool clip, float& uMax, float& vMax);
  void histogram(const EditParams& p, unsigned hist[4][256]);
  // AI local-adjustment masks: RGBA, one channel per layer, in EXIF-oriented image space.
  void setMask(const uint8_t* rgba, int w, int h);
  void clearMask();
  bool hasMask() const { return maskTex_ != 0; }
  void setMaskShow(int channel) { maskShow_ = channel; }  // -1 = off, 4 = recovery
  // AI highlight-recovery overlay: sRGB colour + alpha, EXIF-oriented image space.
  void setRecover(const uint8_t* rgba, int w, int h);
  void clearRecover();
  bool hasRecover() const { return recTex_ != 0; }
  // Photo layers, keyed by ImageLayer::id: colour is EXIF-oriented sRGB, alpha one byte per
  // pixel (nullptr = opaque). The source is re-composited only when the layers change.
  void setLayerImage(int id, const uint8_t* rgb, int w, int h);
  void setLayerMask(int id, const uint8_t* a, int w, int h);
  void dropLayer(int id);
  void dropAllLayers();
  bool layerReady(int id) const { return lay_.count(id) && lay_.at(id).col; }
  void layerSize(int id, int& w, int& h) const;
  int orientedW() const { return orient_ >= 5 ? srcH_ : srcW_; }
  int orientedH() const { return orient_ >= 5 ? srcW_ : srcH_; }
  // Whole output image rendered at w*h and read back as RGBA (AI style matching).
  void renderSmall(const EditParams& p, int w, int h, std::vector<uint8_t>& rgba);

  // Tiled full-resolution export into an RGB buffer of outW*outH*3 bytes.
  void exportBegin(const EditParams& p, int outW, int outH);
  bool exportStep(uint8_t* rgb);  // one tile per call; true when done
  float exportProgress() const;

 private:
  struct Target {
    unsigned fboA = 0, texA = 0, fboB = 0, texB = 0;
    int aW = 0, aH = 0, bW = 0, bH = 0;
  };
  void ensureTarget(Target& t, int aw, int ah, int bw, int bh);
  void freeTarget(Target& t);
  void updateCurve(const EditParams& p);
  void runPasses(const EditParams& p, const Geometry& g, float o0x, float o0y, float o1x, float o1y, int w, int h,
                 int pad, float step, float fx, bool clip, float bgGrey, Target& t);

  unsigned progA_ = 0, progB_ = 0, vao_ = 0, srcTex_ = 0, curveTex_ = 0;
  Target view_[2], hist_, exp_;
  EditParams curveCache_;
  bool curveValid_ = false;
  int srcW_ = 0, srcH_ = 0, orient_ = 1, maxTex_ = 8192;
  unsigned maskTex_ = 0, recTex_ = 0;
  int maskShow_ = -1;

  struct LayerTex {
    unsigned col = 0, mask = 0;
    int w = 0, h = 0;
  };
  unsigned compose(const EditParams& p);  // texture to develop from: the source, or the composite
  std::map<int, LayerTex> lay_;
  unsigned progC_ = 0, compTex_ = 0, compFbo_ = 0;
  bool src16_ = false, compValid_ = false;
  int layerVer_ = 0, compVer_ = -1;
  ImageLayer compKey_[kMaxImgLayers] = {};

  EditParams expP_;
  int expW_ = 0, expH_ = 0, expTile_ = 0, expTilesX_ = 0, expTilesY_ = 0;
  std::vector<uint8_t> tileBuf_;
};
