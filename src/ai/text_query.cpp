#include "ai/text_query.h"

#include <algorithm>
#include <cmath>

namespace ai {

bool containsThai(const std::string& s) {
  for (size_t i = 0; i + 1 < s.size(); i++)
    if ((unsigned char)s[i] == 0xE0 && ((unsigned char)s[i + 1] == 0xB8 || (unsigned char)s[i + 1] == 0xB9)) return true;
  return false;
}

bool TextQuery::load(std::string& err) {
  std::string d = aiDir();
  if (!mtTok_.load(d + "\\mt_vocab.bin") || !bpe_.load(d + "\\clip_bpe.bin")) {
    err = "missing ai\\mt_vocab.bin or ai\\clip_bpe.bin";
    return false;
  }
  return text_.load(d + "\\clip_b32_text_int8.onnx", false, err) &&
         enc_.load(d + "\\mt_th_en_encoder_int8.onnx", false, err) &&
         dec_.load(d + "\\mt_th_en_decoder_int8.onnx", false, err);
}

void TextQuery::release() {
  enc_.release();
  dec_.release();
  text_.release();
}

std::string TextQuery::translate(const std::string& thai, std::string& err) {
  std::vector<int64_t> src = mtTok_.encode(thai);
  if (src.size() > 128) {
    src.resize(127);
    src.push_back(mtTok_.eos());
  }
  std::vector<int64_t> mask(src.size(), 1);
  const int64_t n = (int64_t)src.size();
  std::vector<Tensor> eo;
  if (!enc_.runEx({{"input_ids", DType::I64, src.data(), {1, n}}, {"attention_mask", DType::I64, mask.data(), {1, n}}},
                  {"last_hidden_state"}, eo, err))
    return std::string();
  const std::vector<float>& h = eo[0].data;
  int64_t hd = eo[0].shape.size() == 3 ? eo[0].shape[2] : 512;
  // Greedy decoding; the decoder recomputes the (short) prefix each step.
  std::vector<int64_t> out{mtTok_.pad()};  // decoder_start_token_id
  for (int step = 0; step < 40; step++) {
    std::vector<Tensor> dout;
    const int64_t t = (int64_t)out.size();
    if (!dec_.runEx({{"encoder_attention_mask", DType::I64, mask.data(), {1, n}},
                     {"input_ids", DType::I64, out.data(), {1, t}},
                     {"encoder_hidden_states", DType::F32, h.data(), {1, n, hd}}},
                    {"logits"}, dout, err))
      return std::string();
    int64_t V = dout[0].shape.back();
    const float* last = &dout[0].data[(size_t)(t - 1) * V];
    int64_t best = -1;
    float bv = -1e30f;
    for (int64_t k = 0; k < V; k++)
      if (k != mtTok_.pad() && last[k] > bv) {
        bv = last[k];
        best = k;
      }
    if (best < 0 || best == mtTok_.eos()) break;
    out.push_back(best);
    size_t m = out.size();
    if (m > 4 && out[m - 1] == out[m - 2] && out[m - 2] == out[m - 3] && out[m - 3] == out[m - 4]) break;  // loop guard
  }
  out.erase(out.begin());
  return mtTok_.decode(out);
}

bool TextQuery::embed(const std::string& query, float* emb, std::string* english, std::string& err) {
  std::string en = query;
  if (containsThai(query)) {
    en = translate(query, err);
    if (en.empty()) return false;
  }
  if (english) *english = en;
  std::vector<int64_t> ids = bpe_.encode("a photo of " + en);
  std::vector<Tensor> out;
  if (!text_.runEx({{"input_ids", DType::I64, ids.data(), {1, (int64_t)ids.size()}}}, {"text_embeds"}, out, err))
    return false;
  if (out[0].data.size() < (size_t)kEmbDim) {
    err = "unexpected text embedding";
    return false;
  }
  double nn = 0;
  for (int k = 0; k < kEmbDim; k++) nn += (double)out[0].data[k] * out[0].data[k];
  float inv = (float)(1.0 / std::sqrt(std::max(nn, 1e-12)));
  for (int k = 0; k < kEmbDim; k++) emb[k] = out[0].data[k] * inv;
  return true;
}

}  // namespace ai
