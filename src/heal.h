// Magic eraser: strokes painted over the base photo are filled in by LaMa (Apache-2.0) and
// kept as small RGBA patches that the renderer lays over the source before everything else.
// EditParams::heals counts the patches in use, so undo / redo only move that number; the
// strokes live in the sidecar and the patches in <folder>\.photoeditor_cache\.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "params.h"

class Renderer;

struct HealStroke {
  float radius;            // in units of the oriented image width
  std::vector<float> pts;  // x, y pairs, EXIF-oriented uv
};

struct HealPatch {
  int x = 0, y = 0, w = 0, h = 0;  // pixels of the oriented photo
  std::vector<uint8_t> rgba;       // sRGB colour + coverage
  uint64_t hash = 0;               // chain: this stroke + every patch before it
  bool ready = false, uploaded = false;
};

class HealStore {
 public:
  ~HealStore();
  // Starts the next missing patch (worker thread) and uploads finished ones; true when the
  // GPU side changed. wait = block until every patch in use exists (command-line export).
  bool sync(const EditParams& p, Renderer& R, const std::string& photo, bool wait = false);
  void clear(Renderer& R);
  // Drops redo patches beyond p.heals, appends the stroke, counts it in p.heals.
  void add(EditParams& p, Renderer& R, HealStroke s);
  bool busy(const EditParams& p) const;  // a patch in use is still being made
  bool ready(int k) const { return k < (int)patches_.size() && patches_[k].ready; }
  const std::vector<HealStroke>& strokes() const { return strokes_; }
  std::string takeError();

  std::string serialize(const EditParams& p) const;  // "heal=r;x,y;..." per stroke in use
  void parse(const std::string& sidecar);

 private:
  void join();
  std::vector<HealStroke> strokes_;
  std::vector<HealPatch> patches_;
  std::thread th_;
  std::atomic<bool> done_{false};
  bool running_ = false;
  int jobK_ = -1;
  HealPatch jobOut_;
  std::string jobErr_, err_;
  int failedK_ = -1;  // do not retry a failed patch every frame
};
