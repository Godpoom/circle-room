#include "ai/highlight.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "image_io.h"
#include "platform_win.h"

namespace ai {

bool Inpainter::load(std::string& err) { return model_.load(aiDir() + "\\lama_inpaint.onnx", false, err); }

bool Inpainter::run(const SmallImage& crop, const std::vector<float>& mask, SmallImage& out, std::string& err) {
  const int S = 512, n = S * S;
  std::vector<float> img((size_t)3 * n);
  for (int i = 0; i < n; i++)
    for (int c = 0; c < 3; c++) img[(size_t)c * n + i] = crop.rgb[(size_t)i * 3 + c] / 255.f;
  std::vector<Tensor> outs;
  if (!model_.runEx({{"image", DType::F32, img.data(), {1, 3, S, S}}, {"mask", DType::F32, mask.data(), {1, 1, S, S}}},
                    {"output"}, outs, err))
    return false;
  if (outs[0].data.size() != (size_t)3 * n) {
    err = "unexpected inpainting output";
    return false;
  }
  out.w = out.h = S;
  out.rgb.resize((size_t)3 * n);
  for (int i = 0; i < n; i++)
    for (int c = 0; c < 3; c++)
      out.rgb[(size_t)i * 3 + c] = (uint8_t)std::clamp((int)std::lround(outs[0].data[(size_t)c * n + i]), 0, 255);
  return true;
}

static bool clipped(const uint8_t* p) { return (p[0] >= 250) + (p[1] >= 250) + (p[2] >= 250) >= 2; }

float blownShare(const SmallImage& img) {
  size_t n = (size_t)img.w * img.h, c = 0;
  for (size_t i = 0; i < n; i++) c += clipped(&img.rgb[i * 3]);
  return n ? (float)c / n : 0.f;
}

// Binary dilation with a (2r+1)^2 square, separable running counts.
static void dilate(std::vector<uint8_t>& m, int W, int H, int r) {
  std::vector<uint8_t> t(m.size());
  for (int y = 0; y < H; y++) {
    int cnt = 0;
    const uint8_t* s = &m[(size_t)y * W];
    for (int x = 0; x <= std::min(r, W - 1); x++) cnt += s[x];
    for (int x = 0; x < W; x++) {
      t[(size_t)y * W + x] = cnt > 0;
      if (x + r + 1 < W) cnt += s[x + r + 1];
      if (x - r >= 0) cnt -= s[x - r];
    }
  }
  for (int x = 0; x < W; x++) {
    int cnt = 0;
    for (int y = 0; y <= std::min(r, H - 1); y++) cnt += t[(size_t)y * W + x];
    for (int y = 0; y < H; y++) {
      m[(size_t)y * W + x] = cnt > 0;
      if (y + r + 1 < H) cnt += t[(size_t)(y + r + 1) * W + x];
      if (y - r >= 0) cnt -= t[(size_t)(y - r) * W + x];
    }
  }
}

// Chamfer (3-4) distance to the nearest pixel outside the mask, in pixels.
static void distanceInside(const std::vector<uint8_t>& m, int W, int H, std::vector<float>& d) {
  const float big = 1e9f;
  d.assign(m.size(), 0.f);
  for (size_t i = 0; i < m.size(); i++) d[i] = m[i] ? big : 0.f;
  auto at = [&](int x, int y) -> float { return (x < 0 || y < 0 || x >= W || y >= H) ? 0.f : d[(size_t)y * W + x]; };
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float& v = d[(size_t)y * W + x];
      if (v == 0.f) continue;
      v = std::min({v, at(x - 1, y) + 3, at(x, y - 1) + 3, at(x - 1, y - 1) + 4, at(x + 1, y - 1) + 4});
    }
  for (int y = H - 1; y >= 0; y--)
    for (int x = W - 1; x >= 0; x--) {
      float& v = d[(size_t)y * W + x];
      if (v == 0.f) continue;
      v = std::min({v, at(x + 1, y) + 3, at(x, y + 1) + 3, at(x + 1, y + 1) + 4, at(x - 1, y + 1) + 4});
    }
  for (float& v : d) v /= 3.f;
}

bool recoverHighlights(const SmallImage& img, Inpainter* lama, const SegResult* seg, Recovery& out) {
  auto t0 = std::chrono::steady_clock::now();
  const int W = img.w, H = img.h;
  const size_t N = (size_t)W * H;
  out = Recovery();
  out.w = W;
  out.h = H;
  out.rgba.assign(N * 4, 0);
  std::vector<uint8_t> mask(N);
  for (size_t i = 0; i < N; i++) mask[i] = clipped(&img.rgb[i * 3]);
  dilate(mask, W, H, 2);  // include the glowing edge
  // Semantic label per pixel, so white sky and white sand become separate areas.
  std::vector<uint8_t> label(N, REG_COUNT);
  if (seg && seg->valid()) {
    for (int y = 0; y < H; y++) {
      int sy = std::min(seg->h - 1, y * seg->h / H);
      for (int x = 0; x < W; x++) {
        if (!mask[(size_t)y * W + x]) continue;
        int sx = std::min(seg->w - 1, x * seg->w / W), best = REG_COUNT, bv = 102;  // >= 40% to count
        for (int r = 0; r < REG_COUNT; r++) {
          int v = seg->prob[(size_t)r * seg->w * seg->h + (size_t)sy * seg->w + sx];
          if (v > bv) bv = v, best = r;
        }
        label[(size_t)y * W + x] = (uint8_t)best;
      }
    }
  }
  // Connected components (4-neighbour, same label).
  std::vector<int> comp(N, -1);
  std::vector<std::vector<int>> pixels;
  std::vector<int> stack;
  for (size_t s = 0; s < N; s++) {
    if (!mask[s] || comp[s] >= 0) continue;
    int id = (int)pixels.size();
    pixels.emplace_back();
    stack.assign(1, (int)s);
    comp[s] = id;
    while (!stack.empty()) {
      int p = stack.back();
      stack.pop_back();
      pixels[id].push_back(p);
      int x = p % W, y = p / W;
      const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (auto& d : nb) {
        int nx = x + d[0], ny = y + d[1];
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
        size_t q = (size_t)ny * W + nx;
        if (mask[q] && comp[q] < 0 && label[q] == label[s]) {
          comp[q] = id;
          stack.push_back((int)q);
        }
      }
    }
  }
  std::vector<float> Y(N);
  for (size_t i = 0; i < N; i++) {
    const uint8_t* p = &img.rgb[i * 3];
    Y[i] = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
  }
  std::vector<float> dist;
  distanceInside(mask, W, H, dist);
  std::vector<float> rec(N * 3, 0.f), alphaF(N, 0.f);
  size_t minArea = std::max<size_t>(16, (size_t)(N * 0.0005));  // tiny specular glints stay white
  std::vector<int> order;
  for (int c = 0; c < (int)pixels.size(); c++)
    if (pixels[c].size() >= minArea) order.push_back(c);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return pixels[a].size() > pixels[b].size(); });
  size_t rebuilt = 0;
  for (int c : order) {
    const std::vector<int>& px = pixels[c];
    int x0 = W, y0 = H, x1 = 0, y1 = 0;
    for (int p : px) {
      x0 = std::min(x0, p % W);
      x1 = std::max(x1, p % W);
      y0 = std::min(y0, p / W);
      y1 = std::max(y1, p / W);
    }
    // Rim: unclipped pixels within 8 px of this area.
    const int R = 8;
    int bx0 = std::max(0, x0 - R), by0 = std::max(0, y0 - R), bx1 = std::min(W - 1, x1 + R), by1 = std::min(H - 1, y1 + R);
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    std::vector<uint8_t> local((size_t)bw * bh, 0);
    for (int p : px) local[(size_t)(p / W - by0) * bw + (p % W - bx0)] = 1;
    dilate(local, bw, bh, R);
    std::vector<float> cr[3];
    int lab = label[px[0]];
    bool labelled = seg && seg->valid() && lab < REG_COUNT;
    for (int pass = 0; pass < 2; pass++) {  // bright rim first, any rim as fallback
      for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
          size_t g = (size_t)(y + by0) * W + (x + bx0);
          if (!local[(size_t)y * bw + x] || mask[g] || (pass == 0 && Y[g] < 0.6f)) continue;
          if (labelled && pass == 0) {  // same kind of surface only: sky from sky, not from trees
            int sx = std::min(seg->w - 1, (x + bx0) * seg->w / W), sy = std::min(seg->h - 1, (y + by0) * seg->h / H);
            if (seg->prob[(size_t)lab * seg->w * seg->h + (size_t)sy * seg->w + sx] < 102) continue;
          }
          const uint8_t* p = &img.rgb[g * 3];
          float yy = std::max(Y[g], 1e-3f);
          for (int k = 0; k < 3; k++) cr[k].push_back(p[k] / 255.f / yy);
        }
      if (cr[0].size() >= 20) break;
    }
    float col[3] = {1.f, 1.f, 1.f};
    if (!cr[0].empty()) {
      for (int k = 0; k < 3; k++) {  // median: ignores small colourful objects (kites, clothes)
        std::nth_element(cr[k].begin(), cr[k].begin() + cr[k].size() / 2, cr[k].end());
        col[k] = cr[k][cr[k].size() / 2];
      }
    }
    if (lab == REG_SKY && cr[0].size() < 20) {
      col[0] = 0.95f; col[1] = 0.99f; col[2] = 1.07f;  // pale sky
    }
    for (float& v : col) v = std::clamp(1.f + (v - 1.f) * 0.7f, 0.6f, 1.4f);  // highlights are paler
    std::vector<float> dd;
    for (int p : px) dd.push_back(dist[p]);
    std::nth_element(dd.begin(), dd.begin() + dd.size() * 95 / 100, dd.end());
    float dmax = std::max(1.f, dd[dd.size() * 95 / 100]);
    size_t fullCount = 0;
    for (int p : px) {
      float t = std::pow(std::clamp(dist[p] / dmax, 0.f, 1.f), 0.6f);
      float lt = 0.90f + 0.09f * t;  // brightest in the middle, like real light
      const uint8_t* o = &img.rgb[(size_t)p * 3];
      bool full = o[0] >= 250 && o[1] >= 250 && o[2] >= 250;
      if (full) {
        fullCount++;
        for (int k = 0; k < 3; k++) rec[(size_t)p * 3 + k] = col[k] * lt;
      } else {
        // Partly clipped: real detail is still there; keep its brightness, fix only the colour.
        float yy = std::max(Y[p], 1e-3f);
        for (int k = 0; k < 3; k++) rec[(size_t)p * 3 + k] = yy * (0.5f * col[k] + 0.5f * (o[k] / 255.f / yy));
      }
      alphaF[p] = 1.f;
    }
    rebuilt += px.size();
    out.areas++;
    // Fine texture from LaMa: luminance detail only, where there is enough context.
    // (only for big pure-white areas; at most 2 runs: LaMa is the slow, memory-hungry part)
    if (lama && lama->ok() && out.textured < 2 && fullCount >= (size_t)(N * 0.005)) {
      int side = std::min(std::max(W, H), (int)(std::max(x1 - x0, y1 - y0) * 1.6f) + 64);
      int sx0 = std::clamp((x0 + x1) / 2 - side / 2, 0, std::max(0, W - side));
      int sy0 = std::clamp((y0 + y1) / 2 - side / 2, 0, std::max(0, H - side));
      int sw = std::min(side, W), sh = std::min(side, H);
      size_t inCrop = 0;
      for (int p : px)
        if (p % W >= sx0 && p % W < sx0 + sw && p / W >= sy0 && p / W < sy0 + sh) inCrop++;
      if ((float)inCrop / ((float)sw * sh) < 0.45f) {
        SmallImage crop, outImg;
        resample(img, (float)sx0, (float)sy0, (float)(sx0 + sw), (float)(sy0 + sh), 512, 512, crop);
        std::vector<float> m512(512 * 512, 0.f);
        for (int y = 0; y < 512; y++)
          for (int x = 0; x < 512; x++) {
            int gx = sx0 + x * sw / 512, gy = sy0 + y * sh / 512;
            m512[(size_t)y * 512 + x] = mask[(size_t)gy * W + gx] ? 1.f : 0.f;
          }
        std::string err;
        if (lama->run(crop, m512, outImg, err)) {
          SmallImage back;
          resample(outImg, 0, 0, 512, 512, sw, sh, back);
          std::vector<float> yl((size_t)sw * sh), blur, tmp;
          for (size_t i = 0; i < yl.size(); i++) {
            const uint8_t* p = &back.rgb[i * 3];
            yl[i] = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
          }
          int br = std::max(2, sw / 120);
          boxMean(yl, sw, sh, br, tmp);
          boxMean(tmp, sw, sh, br, blur);
          double s2 = 0;
          size_t cnt = 0;
          for (int p : px) {
            int lx = p % W - sx0, ly = p / W - sy0;
            if (lx < 0 || ly < 0 || lx >= sw || ly >= sh) continue;
            double dv = yl[(size_t)ly * sw + lx] - blur[(size_t)ly * sw + lx];
            s2 += dv * dv;
            cnt++;
          }
          float sd = cnt ? (float)std::sqrt(s2 / cnt) : 0.f;
          if (sd > 1e-4f) {
            for (int p : px) {
              int lx = p % W - sx0, ly = p / W - sy0;
              if (lx < 0 || ly < 0 || lx >= sw || ly >= sh) continue;
              float det = (yl[(size_t)ly * sw + lx] - blur[(size_t)ly * sw + lx]) / sd * 0.025f;
              float k = 1.f + std::clamp(det, -0.08f, 0.08f);
              for (int ch = 0; ch < 3; ch++) rec[(size_t)p * 3 + ch] *= k;
            }
            out.textured++;
          }
        }
      }
    }
  }
  // Soft edges, then pack sRGB + alpha.
  std::vector<float> a1, a2;
  boxMean(alphaF, W, H, 2, a1);
  boxMean(a1, W, H, 2, a2);
  for (size_t i = 0; i < N; i++) {
    float a = a2[i];
    if (!alphaF[i] && a > 0) {  // outside the area: keep the photo's own colour at the seam
      for (int k = 0; k < 3; k++) out.rgba[i * 4 + k] = img.rgb[i * 3 + k];
    } else {
      for (int k = 0; k < 3; k++) out.rgba[i * 4 + k] = (uint8_t)std::clamp((int)std::lround(rec[i * 3 + k] * 255.f), 0, 255);
    }
    out.rgba[i * 4 + 3] = (uint8_t)std::clamp((int)std::lround(a * 255.f), 0, 255);
  }
  out.blown = (float)rebuilt / N;
  out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

static std::string cachePath(const std::string& imagePath) {
  return dirOf(imagePath) + "\\.photoeditor_cache\\" + fileName(imagePath) + ".recover.png";
}

bool loadRecoverCache(const std::string& imagePath, Recovery& out) {
  std::string cp = cachePath(imagePath);
  unsigned long long s1, s2;
  long long m1, m2;
  if (!fileInfo(imagePath, s1, m1) || !fileInfo(cp, s2, m2) || m2 < m1) return false;  // stale
  int w = 0, h = 0;
  if (!loadRgbaFile(cp, out.rgba, w, h)) return false;
  out.w = w;
  out.h = h;
  return true;
}

bool recoverFile(const std::string& path, const SegResult* seg, bool useCache, Recovery& out, std::string& err) {
  if (useCache && loadRecoverCache(path, out)) return true;
  SmallImage img;
  if (!decodeScaled(path, kRecoverSize, img, nullptr, err)) return false;
  Inpainter lama;
  std::string lerr;
  bool haveLama = lama.load(lerr);  // without it: colour + falloff only
  bool ok = recoverHighlights(img, haveLama ? &lama : nullptr, seg, out);
  lama.release();
  ortUnload();
  if (ok && useCache && out.areas > 0) {
    std::string dir = dirOf(path) + "\\.photoeditor_cache";
    ensureDir(dir);
    setHiddenU(dir);
    writePngRgba(cachePath(path), out.rgba.data(), out.w, out.h, err);
  }
  return ok;
}

}  // namespace ai
