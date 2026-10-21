// "Auto edit (AI)" without references: detect the scene, build a target tone signature
// from the photo itself + a scene profile, solve it into slider values, add region layers
// (sky / people / background) and let CLIP's aesthetic score pick the best of 9 variants.
#pragma once
#include <string>

#include "ai/clip_model.h"
#include "ai/segment.h"
#include "ai/style_match.h"

namespace ai {

// Order = scene prompts in tools/make_prompts.py.
enum Scene { SC_PORTRAIT, SC_GROUP, SC_LANDSCAPE, SC_SUNSET, SC_NIGHT, SC_INDOOR, SC_FOOD, SC_BEACH, SC_SNOW,
             SC_FOREST, SC_CITY, SC_ANIMAL, SC_COUNT };
static_assert(SC_COUNT == kSceneCount, "scene list must match prompts.bin");

struct AutoInputs {
  const float* emb = nullptr;           // CLIP embedding of the photo (scene); null = generic profile
  Clip* clip = nullptr;                 // loaded CLIP to rank candidates; null = take the middle one
  const SegResult* seg = nullptr;       // regions for automatic local layers (optional)
  const std::vector<float>* guide = nullptr;  // grey guide of the segmentation (same as seg's image)
  int guideW = 0, guideH = 0;
};

struct AutoResult {
  EditParams params;
  int scene = -1;
  float sceneConf = 0;
  int candidate = 4;  // 0..8, 4 = plain solution
  float score = 0;
  double ms = 0;
};

// renderer holds the small (<=1024 px) target photo; the result keeps base's geometry.
AutoResult autoEdit(Renderer& renderer, const AutoInputs& in, const EditParams& base);

}  // namespace ai
