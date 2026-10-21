#include "ai/segment.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ai {

// ADE20K class ids per region (see segformer config id2label).
static const std::vector<int> kRegionClasses[REG_COUNT] = {
    {12, 92},                             // person (+ apparel)
    {2},                                  // sky
    {6, 11, 52, 54, 91},                  // road, sidewalk, path, runway, dirt track
    {1, 25, 48, 84, 79, 61},              // building, house, skyscraper, tower, hovel, bridge
    {4, 17, 72, 66},                      // tree, plant, palm, flower
    {9, 29},                              // grass, field
    {21, 26, 60, 128, 109, 113, 104},     // water, sea, river, lake, pool, waterfall, fountain
    {16, 68, 34},                         // mountain, hill, rock
    {13, 46, 94, 3},                      // earth, sand, land, floor
    {20, 80, 83, 102, 76, 103, 90, 127, 116},  // car, bus, truck, van, boat, ship, airplane, bicycle, minibike
    {126},                                // animal
    {0, 5},                               // wall, ceiling
    {120},                                // food
};

bool Segmenter::load(std::string& err) { return model_.load(aiDir() + "\\segformer_b3_ade_fp16.onnx", false, err); }

bool Segmenter::run(const SmallImage& img, SegResult& out, std::string& err) {
  out = SegResult();
  const int L = 640;
  float k = (float)L / std::max(img.w, img.h);
  int W = std::max(32, (int)std::lround(img.w * k / 32.f) * 32), H = std::max(32, (int)std::lround(img.h * k / 32.f) * 32);
  SmallImage r;
  resample(img, 0, 0, (float)img.w, (float)img.h, W, H, r);
  std::vector<float> in((size_t)3 * W * H);
  const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
  for (size_t i = 0, n = (size_t)W * H; i < n; i++)
    for (int c = 0; c < 3; c++) in[c * n + i] = (r.rgb[i * 3 + c] / 255.f - mean[c]) / stdv[c];
  std::vector<Tensor> outs;
  if (!model_.run(in.data(), {1, 3, H, W}, outs, err)) return false;
  const Tensor& t = outs[0];
  if (t.shape.size() != 4 || t.shape[1] != 150) {
    err = "unexpected segmentation output";
    return false;
  }
  int ow = (int)t.shape[3], oh = (int)t.shape[2];
  size_t n = (size_t)ow * oh;
  out.w = ow;
  out.h = oh;
  out.prob.assign(n * REG_COUNT, 0);
  int regionOf[150];
  std::fill(regionOf, regionOf + 150, -1);
  for (int g = 0; g < REG_COUNT; g++)
    for (int c : kRegionClasses[g]) regionOf[c] = g;
  double area[REG_COUNT] = {};
  for (size_t i = 0; i < n; i++) {
    float mx = -1e30f;
    for (int c = 0; c < 150; c++) mx = std::max(mx, t.data[c * n + i]);
    float sum = 0, reg[REG_COUNT] = {};
    for (int c = 0; c < 150; c++) {
      float e = std::exp(t.data[c * n + i] - mx);
      sum += e;
      if (regionOf[c] >= 0) reg[regionOf[c]] += e;
    }
    for (int g = 0; g < REG_COUNT; g++) {
      float p = reg[g] / sum;
      area[g] += p;
      out.prob[g * n + i] = (uint8_t)std::lround(std::clamp(p, 0.f, 1.f) * 255.f);
    }
  }
  for (int g = 0; g < REG_COUNT; g++) out.area[g] = (float)(area[g] / n);
  return true;
}

void makeGuide(const SmallImage& img, std::vector<float>& gray) {
  gray.resize((size_t)img.w * img.h);
  for (size_t i = 0; i < gray.size(); i++) {
    const uint8_t* p = &img.rgb[i * 3];
    gray[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.f;
  }
}

// Mean over a (2r+1)^2 window, clamped at the borders; separable running sums, O(N).
void boxMean(const std::vector<float>& in, int w, int h, int r, std::vector<float>& out) {
  std::vector<float> tmp(in.size());
  out.resize(in.size());
  for (int y = 0; y < h; y++) {
    const float* s = &in[(size_t)y * w];
    float* d = &tmp[(size_t)y * w];
    double acc = 0;
    int cnt = 0;
    for (int x = 0; x <= std::min(r, w - 1); x++) acc += s[x], cnt++;
    for (int x = 0; x < w; x++) {
      d[x] = (float)(acc / cnt);
      int add = x + r + 1, rem = x - r;
      if (add < w) acc += s[add], cnt++;
      if (rem >= 0) acc -= s[rem], cnt--;
    }
  }
  for (int x = 0; x < w; x++) {
    double acc = 0;
    int cnt = 0;
    for (int y = 0; y <= std::min(r, h - 1); y++) acc += tmp[(size_t)y * w + x], cnt++;
    for (int y = 0; y < h; y++) {
      out[(size_t)y * w + x] = (float)(acc / cnt);
      int add = y + r + 1, rem = y - r;
      if (add < h) acc += tmp[(size_t)add * w + x], cnt++;
      if (rem >= 0) acc -= tmp[(size_t)rem * w + x], cnt--;
    }
  }
}

void buildLayerMask(const SegResult& seg, int region, bool invert, float feather, const std::vector<float>& I, int W,
                    int H, uint8_t* rgba, int channel) {
  size_t n = (size_t)W * H;
  std::vector<float> p(n, 0.f);
  if (seg.valid() && region >= 0 && region < REG_COUNT) {
    const uint8_t* src = &seg.prob[(size_t)region * seg.w * seg.h];
    for (int y = 0; y < H; y++) {
      float fy = std::clamp((y + 0.5f) * seg.h / H - 0.5f, 0.f, (float)seg.h - 1);
      int y0 = (int)fy, y1 = std::min(y0 + 1, seg.h - 1);
      float ty = fy - y0;
      for (int x = 0; x < W; x++) {
        float fx = std::clamp((x + 0.5f) * seg.w / W - 0.5f, 0.f, (float)seg.w - 1);
        int x0 = (int)fx, x1 = std::min(x0 + 1, seg.w - 1);
        float tx = fx - x0;
        float a = src[y0 * seg.w + x0] * (1 - tx) + src[y0 * seg.w + x1] * tx;
        float b = src[y1 * seg.w + x0] * (1 - tx) + src[y1 * seg.w + x1] * tx;
        p[(size_t)y * W + x] = (a * (1 - ty) + b * ty) / 255.f;
      }
    }
    // Guided filter (He et al.): snaps the coarse mask to the photo's real edges.
    if (I.size() == n) {
      int r = std::max(2, std::max(W, H) / 256);
      const float eps = 1e-3f;
      std::vector<float> mI, mP, mIP, mII, tmp(n);
      boxMean(I, W, H, r, mI);
      boxMean(p, W, H, r, mP);
      for (size_t i = 0; i < n; i++) tmp[i] = I[i] * p[i];
      boxMean(tmp, W, H, r, mIP);
      for (size_t i = 0; i < n; i++) tmp[i] = I[i] * I[i];
      boxMean(tmp, W, H, r, mII);
      std::vector<float> a(n), b(n);
      for (size_t i = 0; i < n; i++) {
        float var = mII[i] - mI[i] * mI[i], cov = mIP[i] - mI[i] * mP[i];
        a[i] = cov / (var + eps);
        b[i] = mP[i] - a[i] * mI[i];
      }
      std::vector<float> ma, mb;
      boxMean(a, W, H, r, ma);
      boxMean(b, W, H, r, mb);
      for (size_t i = 0; i < n; i++) p[i] = std::clamp(ma[i] * I[i] + mb[i], 0.f, 1.f);
    }
    if (feather > 0.5f) {
      int fr = std::max(1, (int)(feather / 100.f * std::max(W, H) / 60.f));
      std::vector<float> q;
      boxMean(p, W, H, fr, q);
      boxMean(q, W, H, fr, p);
    }
  }
  for (size_t i = 0; i < n; i++) {
    float v = invert ? 1.f - p[i] : p[i];
    rgba[i * 4 + channel] = (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f);
  }
}

bool segmentFile(const std::string& path, SegResult& seg, std::vector<float>& guide, int& W, int& H, std::string& err) {
  SmallImage img;
  if (!decodeScaled(path, 1536, img, nullptr, err)) return false;
  Segmenter s;
  bool ok = s.load(err) && s.run(img, seg, err);
  s.release();
  ortUnload();
  if (!ok) return false;
  makeGuide(img, guide);
  W = img.w;
  H = img.h;
  return true;
}

void buildMasks(const SegResult& seg, const std::vector<float>& guide, int W, int H, const LocalLayer* layers,
                std::vector<uint8_t>& rgba) {
  rgba.assign((size_t)W * H * 4, 0);
  for (int i = 0; i < kMaxLayers; i++)
    if (layers[i].enabled) buildLayerMask(seg, layers[i].region, layers[i].invert != 0, layers[i].feather, guide, W, H, rgba.data(), i);
}

}  // namespace ai