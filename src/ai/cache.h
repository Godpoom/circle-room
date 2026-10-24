// Per-folder analysis cache: ".photoeditor_ai" (scores, embeddings, tone) and
// ".photoeditor_thumbs" (256 px JPEG thumbnails, read on demand by offset).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ai/clip_model.h"
#include "ai/features.h"

namespace ai {

struct PhotoInfo {
  std::string path;  // full path (only the file name is stored on disk)
  uint64_t size = 0;
  int64_t mtime = 0;
  int w = 0, h = 0;
  int64_t shot = 0;
  Features feat;
  int faces = 0;
  float faceSharp = 0, faceArea = 0, eyesOpen = 1;
  bool hasAi = false;  // CLIP ran (ONNX Runtime available)
  float aesthetic = 0;
  float prompts[P_COUNT] = {};
  float emb[kEmbDim] = {};
  ToneSig tone = {};
  uint64_t thumbOff = 0;
  uint32_t thumbLen = 0;
};

bool loadCache(const std::string& folder, std::vector<PhotoInfo>& out);
bool saveCache(const std::string& folder, const std::vector<PhotoInfo>& items);

class ThumbStore {
 public:
  bool openAppend(const std::string& folder);
  bool append(const std::vector<uint8_t>& jpeg, uint64_t& off, uint32_t& len);
  void close();
  ~ThumbStore() { close(); }

 private:
  FILE* f_ = nullptr;
};
bool readThumb(const std::string& folder, uint64_t off, uint32_t len, std::vector<uint8_t>& out);
// Deletes only files this app creates for the folder: the analysis cache, thumbnails and the
// AI overlays in .photoeditor_cache. Photos and .pedit edits are never touched. Returns the count.
int clearFolderCache(const std::string& folder);

}  // namespace ai
