#include "ai/clip_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "platform_win.h"

namespace ai {

bool Clip::load(bool preferGpu, std::string& err) {
  std::vector<unsigned char> b;
  if (!readFileBytes(aiDir() + "\\aesthetic_b32.bin", b) || b.size() != 513 * 4) {
    err = "missing ai\\aesthetic_b32.bin";
    return false;
  }
  head_.resize(513);
  std::memcpy(head_.data(), b.data(), b.size());
  if (!readFileBytes(aiDir() + "\\prompts.bin", b) || b.size() < 12 || std::memcmp(b.data(), "PEPR", 4) != 0) {
    err = "missing ai\\prompts.bin";
    return false;
  }
  uint32_t n, dim;
  std::memcpy(&n, &b[4], 4);
  std::memcpy(&dim, &b[8], 4);
  if (n != 2 * P_COUNT + kSceneCount || dim != kEmbDim || b.size() != 12 + (size_t)n * (32 + dim * 4)) {
    err = "ai\\prompts.bin does not match this build";
    return false;
  }
  prompts_.resize((size_t)n * dim);
  for (uint32_t i = 0; i < n; i++) std::memcpy(&prompts_[(size_t)i * dim], &b[12 + (size_t)i * (32 + dim * 4) + 32], dim * 4);
  // Measured: the int8 model runs ~2x faster on the CPU EP than on DirectML (which also needs
  // ~2 s to start), so the GPU is only used when explicitly requested.
  return model_.load(aiDir() + "\\clip_b32_vision_int8.onnx", preferGpu, err);
}

void Clip::release() { model_.release(); }

void Clip::prep(const SmallImage& img, float* out, float x0, float y0, float side) {
  if (x0 < 0) {  // centre square, as CLIP was trained
    side = (float)std::min(img.w, img.h);
    x0 = (img.w - side) * 0.5f;
    y0 = (img.h - side) * 0.5f;
  }
  SmallImage s;
  resample(img, x0, y0, x0 + side, y0 + side, kClipSize, kClipSize, s);
  const float mean[3] = {0.48145466f, 0.4578275f, 0.40821073f}, stdv[3] = {0.26862954f, 0.26130258f, 0.27577711f};
  const int plane = kClipSize * kClipSize;
  for (int i = 0; i < plane; i++)
    for (int c = 0; c < 3; c++) out[c * plane + i] = (s.rgb[i * 3 + c] / 255.f - mean[c]) / stdv[c];
}

bool Clip::embed(const float* tensors, int n, float* outEmb, std::string& err) {
  std::vector<Tensor> outs;
  if (!model_.run(tensors, {n, 3, kClipSize, kClipSize}, outs, err)) return false;
  const Tensor* e = nullptr;
  for (auto& t : outs)
    if (t.shape.size() == 2 && t.shape[1] == kEmbDim) e = &t;
  if (!e) {
    err = "unexpected CLIP output";
    return false;
  }
  for (int i = 0; i < n; i++) {
    const float* s = &e->data[(size_t)i * kEmbDim];
    double nn = 0;
    for (int k = 0; k < kEmbDim; k++) nn += (double)s[k] * s[k];
    float inv = (float)(1.0 / std::sqrt(std::max(nn, 1e-12)));
    for (int k = 0; k < kEmbDim; k++) outEmb[(size_t)i * kEmbDim + k] = s[k] * inv;
  }
  return true;
}

int Clip::scene(const float* e, float* conf) const {
  float logits[kSceneCount], mx = -1e30f;
  for (int s = 0; s < kSceneCount; s++) {
    const float* p = &prompts_[(size_t)(2 * P_COUNT + s) * kEmbDim];
    float d = 0;
    for (int k = 0; k < kEmbDim; k++) d += p[k] * e[k];
    logits[s] = 100.f * d;
    mx = std::max(mx, logits[s]);
  }
  float sum = 0;
  int best = 0;
  for (int s = 0; s < kSceneCount; s++) {
    logits[s] = std::exp(logits[s] - mx);
    sum += logits[s];
    if (logits[s] > logits[best]) best = s;
  }
  if (conf) *conf = logits[best] / sum;
  return best;
}

float Clip::aesthetic(const float* e) const {
  float s = head_[512];
  for (int k = 0; k < kEmbDim; k++) s += head_[k] * e[k];
  return s;
}

float Clip::prompt(const float* e, int id) const {
  const float* p = &prompts_[(size_t)(2 * id) * kEmbDim];
  const float* q = p + kEmbDim;
  float a = 0, b = 0;
  for (int k = 0; k < kEmbDim; k++) {
    a += p[k] * e[k];
    b += q[k] * e[k];
  }
  return 1.f / (1.f + std::exp(-100.f * (a - b)));  // CLIP logit scale
}

// ---------------------------------------------------------------- YuNet
bool FaceDetector::load(std::string& err) { return model_.load(aiDir() + "\\yunet.onnx", false, err); }

bool FaceDetector::detect(const SmallImage& img, std::vector<Face>& faces, std::string& err) {
  faces.clear();
  const int S = 640;
  float scale = (float)S / std::max(img.w, img.h);
  int w = std::max(1, (int)std::lround(img.w * scale)), h = std::max(1, (int)std::lround(img.h * scale));
  SmallImage r;
  resample(img, 0, 0, (float)img.w, (float)img.h, w, h, r);
  std::vector<float> in((size_t)3 * S * S, 0.f);  // letterboxed top-left, BGR 0..255
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const uint8_t* p = &r.rgb[((size_t)y * w + x) * 3];
      size_t i = (size_t)y * S + x;
      in[i] = p[2];
      in[(size_t)S * S + i] = p[1];
      in[(size_t)2 * S * S + i] = p[0];
    }
  std::vector<Tensor> outs;
  if (!model_.run(in.data(), {1, 3, S, S}, outs, err)) return false;
  auto find = [&](const char* name) -> const Tensor* {
    const auto& names = model_.outputNames();
    for (size_t i = 0; i < names.size(); i++)
      if (names[i] == name) return &outs[i];
    return nullptr;
  };
  std::vector<Face> cand;
  for (int s : {8, 16, 32}) {
    char n1[16], n2[16], n3[16];
    snprintf(n1, sizeof(n1), "cls_%d", s);
    snprintf(n2, sizeof(n2), "obj_%d", s);
    snprintf(n3, sizeof(n3), "bbox_%d", s);
    const Tensor *cls = find(n1), *obj = find(n2), *box = find(n3);
    if (!cls || !obj || !box) continue;
    int cols = S / s, count = (int)cls->data.size();
    for (int i = 0; i < count; i++) {
      float sc = std::sqrt(std::clamp(cls->data[i], 0.f, 1.f) * std::clamp(obj->data[i], 0.f, 1.f));
      if (sc < 0.6f) continue;
      int rr = i / cols, cc = i % cols;
      const float* b = &box->data[(size_t)i * 4];
      float cx = (cc + b[0]) * s, cy = (rr + b[1]) * s, bw = std::exp(b[2]) * s, bh = std::exp(b[3]) * s;
      cand.push_back({(cx - bw / 2) / scale, (cy - bh / 2) / scale, bw / scale, bh / scale, sc});
    }
  }
  std::sort(cand.begin(), cand.end(), [](const Face& a, const Face& b) { return a.score > b.score; });
  for (const Face& c : cand) {  // NMS
    bool keep = true;
    for (const Face& k : faces) {
      float ix = std::max(0.f, std::min(c.x + c.w, k.x + k.w) - std::max(c.x, k.x));
      float iy = std::max(0.f, std::min(c.y + c.h, k.y + k.h) - std::max(c.y, k.y));
      float inter = ix * iy, uni = c.w * c.h + k.w * k.h - inter;
      if (uni > 0 && inter / uni > 0.3f) keep = false;
    }
    if (keep) faces.push_back(c);
    if (faces.size() >= 8) break;
  }
  return true;
}

}  // namespace ai
