// Text -> CLIP embedding for photo search. Thai is first translated to English with
// Helsinki-NLP opus-mt-th-en (greedy decoding), then encoded by the CLIP ViT-B/32 text tower,
// so it lands in the same space as the image embeddings already in the folder cache.
#pragma once
#include <string>

#include "ai/clip_model.h"
#include "ai/ort.h"
#include "ai/tokenizers.h"

namespace ai {

bool containsThai(const std::string& utf8);

class TextQuery {
 public:
  bool load(std::string& err);
  void release();
  bool ok() const { return text_.ok(); }
  std::string translate(const std::string& thai, std::string& err);
  // emb: kEmbDim floats, L2-normalised. english receives the text actually encoded.
  bool embed(const std::string& query, float* emb, std::string* english, std::string& err);

 private:
  UnigramTokenizer mtTok_;
  ClipBpe bpe_;
  Model enc_, dec_, text_;
};

}  // namespace ai
