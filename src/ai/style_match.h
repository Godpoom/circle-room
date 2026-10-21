// "Edit like my references": finds slider values (EditParams) whose rendered result
// matches the references' tone signature. The result stays fully editable.
#pragma once
#include <string>
#include <vector>

#include "ai/cache.h"
#include "params.h"
#include "renderer.h"

namespace ai {

struct StyleRef {
  std::string path;
  bool hasAi = false;
  float emb[kEmbDim] = {};
  ToneSig tone = {};
};

struct StyleSet {
  std::string name;
  std::vector<StyleRef> refs;
  bool save(const std::string& file) const;
  bool load(const std::string& file);
  static StyleSet fromPhotos(const std::string& name, const std::vector<PhotoInfo>& refs);
};
std::string stylesDir();  // <exe dir>\styles
std::vector<std::string> listStyleFiles();

enum StyleGroup : unsigned {
  SG_LIGHT = 1,   // exposure, contrast, highlights, shadows, whites, blacks
  SG_COLOR = 2,   // white balance, vibrance, saturation
  SG_HSL = 4,     // colour mixer
  SG_GRADE = 8,   // colour grading (shadows / highlights)
  SG_CURVE = 16,  // master tone curve
  SG_ALL = 31
};

struct StyleResult {
  EditParams params;  // full-strength result; geometry copied from the base
  float lossBefore = 0, lossAfter = 0;
  int evals = 0;
  double ms = 0;
  std::vector<float> weights;  // influence of each reference
};

// renderer must hold the (small) target image. emb may be null: then all references weigh the same.
StyleResult solveStyle(Renderer& renderer, const StyleSet& set, const float* emb, const EditParams& base, unsigned groups);
// Same optimisation towards an explicit target signature (used by auto edit).
struct SolveOptions {
  float reg = 0.004f;     // pull towards neutral sliders
  float minSat = -60.f;   // lower bound for vibrance / saturation
};
StyleResult solveToTarget(Renderer& renderer, const ToneSig& target, const EditParams& base, unsigned groups,
                          const SolveOptions& opt = SolveOptions());
// default + s * (opt - default) for everything except geometry (taken from base).
EditParams blendStyle(const EditParams& base, const EditParams& opt, float strength);

}  // namespace ai
