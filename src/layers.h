// Photo layers, CPU side: loading the layer photos, AI cut-out masks and eraser / restore
// strokes. Placement and look live in EditParams::img; everything bulky lives here, keyed by
// ImageLayer::id so undo / reorder never mixes two layers up.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ai/segment.h"
#include "params.h"

class Renderer;

// Called from worker threads when a layer finishes a stage (the GUI wakes its event loop).
extern void (*g_layerWake)();

struct Stroke {
  int erase;               // 1 = erase, 0 = restore
  float radius, hard;      // radius in layer-width units, hardness 0..1
  std::vector<float> pts;  // x, y pairs in layer uv
};

struct LayerAsset {
  int id = 0;
  std::string path, err;
  std::thread th;
  std::atomic<int> stage{0};  // 1 decoding, 2 pixels ready, 3 segmenting, 4 done, -1 failed
  std::vector<uint8_t> rgb;   // oriented sRGB, freed once on the GPU
  int w = 0, h = 0;
  bool uploaded = false, segOk = false, maskOn = false;
  ai::SegResult seg;
  std::vector<float> guide;
  int gw = 0, gh = 0;
  std::vector<uint8_t> thumb;  // small RGB: list thumbnail and colour matching
  int tw = 0, th2 = 0;
  unsigned thumbTex = 0;
  // mask state
  std::vector<float> base, alpha;  // cut-out, then cut-out + strokes (1 = visible)
  int mw = 0, mh = 0, builtKeep = -1, builtGen = -1, doneStrokes = 0;
  float builtFeather = -1.f;
  size_t donePts = 0;
  ~LayerAsset() {
    if (th.joinable()) th.join();
  }
};

class LayerStore {
 public:
  // Loads new layers (worker threads), uploads finished ones and rebuilds masks.
  // wait = block until every layer is loaded and segmented (export from the command line).
  // Returns true when anything on the GPU changed.
  bool sync(const EditParams& p, Renderer& R, bool wait = false);
  void clear(Renderer& R);  // another photo was opened
  LayerAsset* asset(int id);
  bool busy(const EditParams& p);
  int nextId(const EditParams& p) const;

  // Brush: a new stroke drops the redo strokes beyond l.strokes, then l.strokes counts it.
  void beginStroke(ImageLayer& l, int erase, float radius, float hard, float x, float y);
  void extendStroke(const ImageLayer& l, float x, float y);

  // Sidecar lines "stroke<id>=e,r,h;x,y;..." for the strokes in use.
  std::string serializeStrokes(const EditParams& p) const;
  void parseStrokes(const std::string& sidecar);

  // Exposure / temperature / saturation that bring the layer close to the base photo's light.
  bool matchColours(const std::string& basePath, ImageLayer& l);

 private:
  std::map<int, std::unique_ptr<LayerAsset>> assets_;
  std::map<int, std::vector<Stroke>> strokes_;
  std::map<int, int> gen_;  // bumped whenever a layer's stroke list is cut back
};

// Oriented 8-bit sRGB copy of a decoded photo, long edge <= maxDim.
bool loadLayerPixels(const std::string& path, int maxDim, std::vector<uint8_t>& rgb, int& w, int& h, std::string& err);
