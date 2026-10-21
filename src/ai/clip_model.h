// CLIP ViT-B/32 image encoder + LAION aesthetic head + precomputed prompt embeddings.
#pragma once
#include <string>
#include <vector>

#include "ai/ort.h"
#include "ai/wic_decode.h"

namespace ai {

constexpr int kEmbDim = 512;
constexpr int kClipSize = 224;
constexpr int kClipTensor = 3 * kClipSize * kClipSize;

// Must match the order in tools/make_prompts.py.
enum PromptId { P_COMPOSITION, P_SUBJECT, P_LIGHTING, P_SHARPNESS, P_EXPOSURE, P_LEVEL, P_QUALITY, P_EYES, P_COUNT };
// Scene prompts follow the pairs in prompts.bin (see Scene in ai/auto_edit.h).
constexpr int kSceneCount = 12;

class Clip {
 public:
  bool load(bool preferGpu, std::string& err);
  void release();
  bool ok() const { return model_.ok(); }
  bool onGpu() const { return model_.onGpu(); }
  // CLIP preprocessing of a source rectangle (default: centre square) into CHW floats.
  static void prep(const SmallImage& img, float* out, float x0 = -1, float y0 = 0, float side = 0);
  // n tensors -> n L2-normalised embeddings.
  bool embed(const float* tensors, int n, float* outEmb, std::string& err);
  float aesthetic(const float* e) const;    // LAION scale, roughly 1..10
  float prompt(const float* e, int id) const;  // P(positive text) 0..1
  // Most likely scene (softmax over the scene prompts); conf = its probability.
  int scene(const float* e, float* conf) const;

 private:
  Model model_;
  std::vector<float> head_;     // 512 weights + bias
  std::vector<float> prompts_;  // [2 * P_COUNT][512]
};

struct Face {
  float x, y, w, h, score;
};

class FaceDetector {
 public:
  bool load(std::string& err);  // CPU: the model is tiny
  void release() { model_.release(); }
  bool ok() const { return model_.ok(); }
  bool detect(const SmallImage& img, std::vector<Face>& faces, std::string& err);

 private:
  Model model_;
};

}  // namespace ai
