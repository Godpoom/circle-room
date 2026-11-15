// Auto edit many photos at once. For each photo the AI picks a look for its scene (same as the
// editor's Auto edit, including region layers and highlight rebuild), saves it as the photo's
// .pedit, and can also export a full-size JPEG. Decoding, CLIP, segmentation, the highlight
// rebuild and JPEG encoding run on a worker thread; only the short GPU solve and render run on
// the UI thread, so the app stays usable while a batch runs.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "image_io.h"
#include "params.h"
#include "ui_cull.h"

struct BatchHost {
  void* hwnd = nullptr;
  float scale = 1.f;
  std::function<Renderer*()> renderer;               // the small AI renderer (export renders through it too)
  std::function<void(const std::string&)> toast;
  std::function<void(const std::string&)> edited;    // a photo's .pedit was rewritten
  std::string lastDir;
};

class AutoBatch {
 public:
  ~AutoBatch() { shutdown(); }
  void open(std::vector<std::string> photos);  // shows the options dialog for these photos
  void draw(BatchHost& h);                      // dialog + progress card; every frame, any mode
  void pump(BatchHost& h);                      // advances the job; every frame
  bool busy() const { return active_; }
  void shutdown();

 private:
  void start(BatchHost& h);
  void finish(BatchHost& h);
  void next();
  void runWorker(std::function<void()> fn);

  // options
  std::vector<std::string> pending_;
  bool openDlg_ = false, skipEdited_ = true, exportJpg_ = false;
  float strength_ = 100.f;
  // job
  std::vector<std::string> items_;
  std::string outDir_;
  bool active_ = false, cancel_ = false;
  size_t pos_ = 0;
  int stage_ = 0;  // 0 next photo, 1 AI prepare (worker), 2 full decode (worker), 3 encode (worker)
  int edited_ = 0, skipped_ = 0, failed_ = 0, exported_ = 0;
  std::thread th_;
  std::atomic<bool> ready_{false};
  // current photo
  EditParams base_, params_;
  std::string sidecar_, extra_;  // old .pedit text, and its stroke / heal lines kept on rewrite
  // worker results
  AutoModels models_;
  bool modelsTried_ = false, modelsOk_ = false, prepOk_ = false, hasEmb_ = false, segOk_ = false, ok_ = false;
  float emb_[ai::kEmbDim] = {};
  ai::SegResult seg_;
  std::vector<float> guide_;
  int gw_ = 0, gh_ = 0, w_ = 0, h_ = 0;
  ImageData img_;
  std::vector<uint8_t> rgb_;
  std::string err_, out_;
};
