// Semantic regions (person, sky, road, ...) from UperNet-ConvNeXt-T (ADE20K, fp16, CPU) and
// edge-aware layer masks for local adjustments.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ai/ort.h"
#include "ai/wic_decode.h"
#include "params.h"

namespace ai {

// Friendly groups of ADE20K classes. The order is stored in sidecars: append only.
enum Region {
  REG_PERSON, REG_SKY, REG_ROAD, REG_BUILDING, REG_TREES, REG_GRASS, REG_WATER, REG_MOUNTAIN,
  REG_GROUND, REG_VEHICLE, REG_ANIMAL, REG_WALLS, REG_FOOD,
  REG_FACE,  // not an ADE20K class: YuNet face boxes shaped by the person map (see addFaces)
  REG_COUNT
};

struct FaceBox { float x, y, w, h; };  // normalized to the segmented image

struct SegResult {
  int w = 0, h = 0;                       // low-res probability maps (model output size)
  std::vector<uint8_t> prob;              // REG_COUNT maps of w*h, 0..255
  float area[REG_COUNT] = {};             // share of the image per region
  std::vector<FaceBox> faces;             // REG_FACE is drawn from these at mask resolution
  bool valid() const { return w > 0; }
};

class Segmenter {
 public:
  bool load(std::string& err);
  void release() { model_.release(); }
  bool ok() const { return model_.ok(); }
  bool run(const SmallImage& img, SegResult& out, std::string& err);  // img: any size, oriented

 private:
  Model model_;
};

// Detects faces (YuNet) and fills seg.faces / seg.area[REG_FACE]. Missing model: no faces, no error.
void addFaces(const SmallImage& img, SegResult& seg);

// Mean over a (2r+1)^2 window (clamped at borders), O(N).
void boxMean(const std::vector<float>& in, int w, int h, int r, std::vector<float>& out);
// Grey guide image (0..1) for edge-aware refinement.
void makeGuide(const SmallImage& img, std::vector<float>& gray);
// Writes one channel (stride 4) of a W*H RGBA mask: region probability, refined with a
// guided filter on the photo, optionally feathered (0..100) and inverted.
void buildLayerMask(const SegResult& seg, int region, bool invert, float feather, const std::vector<float>& guide,
                    int W, int H, uint8_t* rgba, int channel);

// Photo-layer cut-out: union of the regions in keepBits (1 << Region), edge-snapped and
// tightened, softened by feather (0..100). out: W*H floats 0..1.
void buildKeepMask(const SegResult& seg, int keepBits, float feather, const std::vector<float>& guide, int W, int H,
                   std::vector<float>& out);

// Blocking helpers (worker thread / CLI): decode at 1536 px, segment, keep the grey guide.
bool segmentFile(const std::string& path, SegResult& seg, std::vector<float>& guide, int& W, int& H, std::string& err);
// All four layer channels of the mask (disabled layers stay 0).
void buildMasks(const SegResult& seg, const std::vector<float>& guide, int W, int H, const LocalLayer* layers,
                std::vector<uint8_t>& rgba);

}  // namespace ai
