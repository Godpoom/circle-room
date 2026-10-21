// Photo culling: background analysis (decode -> features -> CLIP/YuNet) and ranking.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ai/cache.h"

namespace ai {

// Explanations shown next to each photo (bit flags; translated in the UI).
enum Reason : uint32_t {
  R_BLURRY = 1u << 0,
  R_FACE_BLUR = 1u << 1,
  R_EYES_CLOSED = 1u << 2,
  R_OVEREXPOSED = 1u << 3,
  R_UNDEREXPOSED = 1u << 4,
  R_NOISY = 1u << 5,
  R_TILTED = 1u << 6,
  R_EDGE_CUT = 1u << 7,
  R_OFF_BALANCE = 1u << 8,
  R_GOOD_COMP = 1u << 9,
  R_SHARP = 1u << 10,
  R_GOOD_LIGHT = 1u << 11,
  R_DUPLICATE = 1u << 12,
  R_LIKE_REF = 1u << 13,
  R_COUNT_BITS = 14
};

class Analyzer {
 public:
  ~Analyzer();
  // Folder mode reuses and updates the folder cache; file mode (references) does not.
  void startFolder(const std::string& folder, bool preferGpu);
  void startFiles(const std::vector<std::string>& files, bool preferGpu);
  bool running() const { return running_; }
  float progress() const;
  int done() const { return done_; }
  int total() const { return total_; }
  void cancel() { cancel_ = true; }
  // True once after the job ends; then take the results.
  bool poll();
  std::vector<PhotoInfo> takeResults();
  std::string message() const;  // device used, or why AI was unavailable
  void wait();

 private:
  void run(bool preferGpu);
  std::string folder_;
  std::vector<std::string> files_;
  std::vector<PhotoInfo> results_;
  std::thread th_;
  std::atomic<bool> running_{false}, cancel_{false}, finished_{false};
  std::atomic<int> done_{0}, total_{0};
  mutable std::mutex msgMutex_;
  std::string msg_;
};

// Analyses one already-decoded image (used by the style tools and the analyzer).
struct AnalyzeModels {
  Clip clip;
  FaceDetector face;
};
void analyzeImage(const SmallImage& img, AnalyzeModels* models, PhotoInfo& out, std::vector<uint8_t>* thumbJpeg);

struct RankSettings {
  float wTech = 30, wAes = 40, wComp = 30;
  bool refMode = false;
  float alpha = 50;  // 100 = quality only, 0 = similarity only
  bool dedupe = true;
};

struct RankEntry {
  int idx = 0;
  float score = 0;  // 0..100 shown to the user
  float tech = 0, aes = 0, comp = 0, sim = 0;  // 0..1 parts
  int group = 0, groupSize = 1;
  bool groupBest = true;
  uint32_t reasons = 0;
};

// Near-duplicate groups (bursts): returns a group id per photo.
std::vector<int> groupDuplicates(const std::vector<PhotoInfo>& photos);
// Sorted best first; with dedupe only the best of each group is kept.
std::vector<RankEntry> rankPhotos(const std::vector<PhotoInfo>& photos, const std::vector<int>& groups,
                                  const std::vector<PhotoInfo>& refs, const RankSettings& s);

}  // namespace ai
