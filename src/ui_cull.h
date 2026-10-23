// "AI Cull" screen: analyse a folder, rank it, show the top N, reference mode,
// copy picks and batch-apply a reference style.
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ai/cull.h"
#include "ai/style_match.h"
#include "ai/text_query.h"
#include "ai/auto_edit.h"
#include "ai/highlight.h"
#include "image_io.h"
#include "renderer.h"

struct CullHost {
  void* hwnd = nullptr;
  float scale = 1.f;
  std::function<void(const std::string&)> openInEditor;
  std::function<void(const std::string&)> toast;
  std::function<Renderer*()> aiRenderer;  // small, lazily created renderer for style matching
};

// Saved styles (reference sets), shared by the cull screen and the editor's AI Style panel.
class StyleLibrary {
 public:
  void reload();
  const std::vector<ai::StyleSet>& sets() const { return sets_; }
  int selected = 0;
  bool save(const ai::StyleSet& s, std::string& err);  // writes styles\<name>.lrs and selects it
  void startCreate(const std::string& name, const std::vector<std::string>& files);
  bool creating() const { return an_.running(); }
  float progress() const { return an_.progress(); }
  // Call every frame; returns true once when a style was created.
  bool pump(std::string& err);

 private:
  std::vector<ai::StyleSet> sets_;
  ai::Analyzer an_;
  std::string pendingName_;
};

// Decodes a photo small, uploads it to the renderer and solves the style.
bool aiStyleSolveFile(Renderer& r, const ai::StyleSet& set, const std::string& path, const float* emb,
                      const EditParams& base, unsigned groups, ai::StyleResult& out, std::string& err);
// Embedding of a photo from its folder's analysis cache (false if not analysed).
bool aiCachedEmbedding(const std::string& path, float* emb);

// Models kept loaded while auto-editing one or many photos.
struct AutoModels {
  ai::Clip clip;
  ai::Segmenter seg;
  bool load(std::string& err);  // CLIP is required, segmentation optional
  void release();
};
// Auto edit of a file: decodes small into r, finds the scene (cached embedding or CLIP),
// regions (given or computed when wantLayers) and solves. Result keeps base's geometry.
bool aiAutoEditFile(Renderer& r, AutoModels& m, const std::string& path, const float* emb, const ai::SegResult* seg,
                    const std::vector<float>* guide, int gw, int gh, bool wantLayers, const EditParams& base,
                    ai::AutoResult& out, std::string& err, bool recover = false);
const char* sceneName(int scene);

class CullView {
 public:
  void draw(CullHost& h, StyleLibrary& lib);
  void pump(CullHost& h, StyleLibrary& lib);
  bool busy() const;
  void onDrop(const std::vector<std::string>& paths);
  void shutdown();

 private:
  struct Thumb {
    unsigned tex = 0;
    int w = 0, h = 0, lastFrame = 0;
  };
  void setFolder(const std::string& f);
  void addRefs(const std::vector<std::string>& files);
  unsigned thumb(int photo, int& w, int& h);
  void clearThumbs();
  void drawControls(CullHost& h, StyleLibrary& lib);
  void drawGrid(CullHost& h);
  void drawDetail(CullHost& h);
  void drawSearchBar(CullHost& h);
  void startSearch(const std::string& q);
  void applySearch();
  int shownCount() const;
  // Saving the picks: the shown top N minus the photos the user left out.
  std::vector<int> picks() const;
  void drawSaveBar(CullHost& h);
  void startSave(CullHost& h, const std::string& dst);
  void pumpSave(CullHost& h);
  std::unordered_set<int> excluded_;  // photo indices left out of the picks
  bool openSave_ = false, saveRender_ = true;
  struct SaveJob {
    bool active = false, render = true;
    std::vector<int> items;
    size_t pos = 0;
    int done = 0, failed = 0, stage = 0;  // 0 next photo, 1 decoding, 3 encoding
    std::string dst, out, sidecar, err;
    EditParams params;
    ImageData img;
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    bool ok = false;
    std::thread th;
    std::atomic<bool> ready{false};
  } save_;

  std::string folder_, device_;
  ai::Analyzer an_, refAn_;
  std::vector<ai::PhotoInfo> photos_, refs_;
  std::vector<int> groups_;
  std::vector<std::string> refPaths_;
  size_t refsSubmitted_ = 0;
  ai::RankSettings rs_;
  int topN_ = 10;
  bool showAll_ = false, rankDirty_ = true, wantFrames_ = false;
  std::vector<ai::RankEntry> ranked_;
  int selected_ = -1;  // index into ranked_
  std::unordered_map<int, Thumb> thumbs_;
  int frame_ = 0, loadsThisFrame_ = 0;
  bool openSaveStyle_ = false;
  char nameBuf_[64] = {};
  // batch "edit like references"
  bool batchActive_ = false;
  std::vector<int> batchItems_;
  size_t batchPos_ = 0;
  int batchSource_ = -1;  // -1 = current references, else library index
  float batchStrength_ = 80.f;
  ai::StyleSet batchSet_;
  bool batchAuto_ = false;  // batch runs "auto edit" instead of a reference style
  AutoModels autoModels_;
  // text / similarity search
  std::vector<ai::RankEntry> qualityRanked_, allRanked_;
  char query_[256] = {};
  bool searchActive_ = false;
  std::string searchLabel_, searchEnglish_, searchErr_;
  float qEmb_[ai::kEmbDim] = {};
  float strict_ = 40.f;
  int searchMatches_ = 0;
  ai::TextQuery textQ_;
  std::thread searchTh_;
  std::atomic<bool> searchDone_{false};
  bool searchRunning_ = false, searchOk_ = false;
  std::string pendingQuery_;
  float pendingEmb_[ai::kEmbDim] = {};
  std::chrono::steady_clock::time_point lastSearch_;
};
