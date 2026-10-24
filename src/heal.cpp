#include "heal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "ai/highlight.h"
#include "ai/ort.h"
#include "ai/segment.h"
#include "image_io.h"
#include "layers.h"
#include "platform_win.h"
#include "renderer.h"

static uint64_t fnv(uint64_t h, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

static uint64_t strokeHash(uint64_t prev, const HealStroke& s, int W, int H, const std::string& photo) {
  unsigned long long size = 0;
  long long mtime = 0;
  fileInfo(photo, size, mtime);  // editing the photo outside invalidates every patch
  const int kVersion = 3;  // bump when the fill changes, so cached patches are remade
  uint64_t h = fnv(prev ? prev : 1469598103934665603ull, &kVersion, sizeof(kVersion));
  h = fnv(h, &W, sizeof(W));
  h = fnv(h, &H, sizeof(H));
  h = fnv(h, &size, sizeof(size));
  h = fnv(h, &mtime, sizeof(mtime));
  h = fnv(h, &s.radius, sizeof(s.radius));
  return fnv(h, s.pts.data(), s.pts.size() * sizeof(float));
}

static std::string patchPath(const std::string& photo, uint64_t hash) {
  char hex[24];
  snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
  return dirOf(photo) + "\\.photoeditor_cache\\" + fileName(photo) + ".heal-" + hex + ".png";
}

// Pixel rectangle a stroke can touch: its dabs plus room for the dilation and soft edge.
static void strokeRect(const HealStroke& s, int W, int H, int& x0, int& y0, int& x1, int& y1) {
  float r = std::max(1.f, s.radius * W), m = r + std::max(10.f, r * 0.75f);
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
  for (size_t i = 0; i + 1 < s.pts.size(); i += 2) {
    minx = std::min(minx, s.pts[i] * W);
    maxx = std::max(maxx, s.pts[i] * W);
    miny = std::min(miny, s.pts[i + 1] * H);
    maxy = std::max(maxy, s.pts[i + 1] * H);
  }
  x0 = std::clamp((int)std::floor(minx - m), 0, W - 1);
  y0 = std::clamp((int)std::floor(miny - m), 0, H - 1);
  x1 = std::clamp((int)std::ceil(maxx + m), x0 + 1, W);
  y1 = std::clamp((int)std::ceil(maxy + m), y0 + 1, H);
}

// The fill for one stroke. img: the oriented photo with every earlier patch already applied.
static bool makePatch(const ai::SmallImage& img, const HealStroke& s, ai::Inpainter& lama, HealPatch& out,
                      std::string& err) {
  const int W = img.w, H = img.h;
  int rx0, ry0, rx1, ry1;
  strokeRect(s, W, H, rx0, ry0, rx1, ry1);
  int rw = rx1 - rx0, rh = ry1 - ry0;
  // Binary stroke mask in the rectangle, then grown a little: objects have soft edges and
  // shadows the brush rarely covers exactly.
  float r = std::max(1.f, s.radius * W);
  std::vector<float> m((size_t)rw * rh, 0.f);
  auto dab = [&](float cx, float cy, float rad) {
    int ax = std::max(rx0, (int)(cx - rad)), bx = std::min(rx1 - 1, (int)(cx + rad) + 1);
    int ay = std::max(ry0, (int)(cy - rad)), by = std::min(ry1 - 1, (int)(cy + rad) + 1);
    for (int y = ay; y <= by; y++)
      for (int x = ax; x <= bx; x++) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        if (dx * dx + dy * dy <= rad * rad) m[(size_t)(y - ry0) * rw + (x - rx0)] = 1.f;
      }
  };
  float grow = r + std::max(3.f, r * 0.25f), step = std::max(1.f, r * 0.3f);
  size_t n = s.pts.size() / 2;
  for (size_t k = 0; k < n; k++) {
    float x = s.pts[k * 2] * W, y = s.pts[k * 2 + 1] * H;
    if (k == 0) {
      dab(x, y, grow);
      continue;
    }
    float px = s.pts[k * 2 - 2] * W, py = s.pts[k * 2 - 1] * H;
    int steps = std::max(1, (int)std::ceil(std::hypot(x - px, y - py) / step));
    for (int i = 1; i <= steps; i++) dab(px + (x - px) * i / steps, py + (y - py) * i / steps, grow);
  }
  int bx0 = rw, by0 = rh, bx1 = -1, by1 = -1;
  for (int y = 0; y < rh; y++)
    for (int x = 0; x < rw; x++)
      if (m[(size_t)y * rw + x] > 0) {
        bx0 = std::min(bx0, x);
        bx1 = std::max(bx1, x);
        by0 = std::min(by0, y);
        by1 = std::max(by1, y);
      }
  if (bx1 < 0) {
    err = "empty stroke";
    return false;
  }
  bx0 += rx0, bx1 += rx0, by0 += ry0, by1 += ry0;
  // Context: twice the object, at least 512 px, so small objects are filled at full detail.
  int side = std::max(512, 2 * std::max(bx1 - bx0 + 1, by1 - by0 + 1));
  int cw = std::min(side, W), ch = std::min(side, H);
  int cx0 = std::clamp((bx0 + bx1) / 2 - cw / 2, 0, W - cw), cy0 = std::clamp((by0 + by1) / 2 - ch / 2, 0, H - ch);
  ai::SmallImage crop, filled, back;
  ai::resample(img, (float)cx0, (float)cy0, (float)(cx0 + cw), (float)(cy0 + ch), 512, 512, crop);
  std::vector<float> m512(512 * 512, 0.f);
  for (int y = 0; y < 512; y++)
    for (int x = 0; x < 512; x++) {
      int gx = cx0 + (int)((x + 0.5f) * cw / 512), gy = cy0 + (int)((y + 0.5f) * ch / 512);
      if (gx >= rx0 && gx < rx1 && gy >= ry0 && gy < ry1) m512[(size_t)y * 512 + x] = m[(size_t)(gy - ry0) * rw + (gx - rx0)];
    }
  // Grow the mask by one 512-px cell so downscaled edges never leak the object back in.
  std::vector<float> g512 = m512;
  for (int y = 0; y < 512; y++)
    for (int x = 0; x < 512; x++)
      if (m512[(size_t)y * 512 + x] > 0)
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int xx = std::clamp(x + dx, 0, 511), yy = std::clamp(y + dy, 0, 511);
            g512[(size_t)yy * 512 + xx] = 1.f;
          }
  if (!lama.run(crop, g512, filled, err)) return false;
  ai::resample(filled, 0, 0, 512, 512, cw, ch, back);
  const size_t rn = (size_t)rw * rh;
  // Work in the patch rectangle: original photo (o) and LaMa fill (f), linear 0..255 floats.
  std::vector<float> o(rn * 3), f(rn * 3);
  for (int y = 0; y < rh; y++)
    for (int x = 0; x < rw; x++) {
      int gx = rx0 + x, gy = ry0 + y;
      const uint8_t* op = &img.rgb[((size_t)gy * W + gx) * 3];
      bool inCrop = gx >= cx0 && gx < cx0 + cw && gy >= cy0 && gy < cy0 + ch;
      const uint8_t* fp = inCrop ? &back.rgb[((size_t)(gy - cy0) * cw + (gx - cx0)) * 3] : op;
      for (int c = 0; c < 3; c++) {
        o[((size_t)y * rw + x) * 3 + c] = op[c];
        f[((size_t)y * rw + x) * 3 + c] = fp[c];
      }
    }
  // 1) Seam: LaMa's colours drift a little from the photo. Measure the difference on a thin ring
  // just outside the mask and spread it smoothly over the fill (push-pull membrane), so the
  // fill meets the photo with no visible step in tone or colour.
  int ringW = std::max(2, (int)(r * 0.12f));
  std::vector<float> near;
  ai::boxMean(m, rw, rh, ringW, near);
  std::vector<float> dv(rn * 3, 0.f), dw(rn, 0.f);
  for (size_t i = 0; i < rn; i++)
    if (m[i] == 0.f && near[i] > 0.f) {
      dw[i] = 1.f;
      for (int c = 0; c < 3; c++) dv[i * 3 + c] = o[i * 3 + c] - f[i * 3 + c];
    }
  {  // tone drift only: smooth the ring's differences so texture / blur differences cancel out
    int sr = std::max(3, ringW * 2);
    std::vector<float> wsum, ch(rn), chs;
    ai::boxMean(dw, rw, rh, sr, wsum);
    for (int c = 0; c < 3; c++) {
      for (size_t i = 0; i < rn; i++) ch[i] = dv[i * 3 + c] * dw[i];
      ai::boxMean(ch, rw, rh, sr, chs);
      for (size_t i = 0; i < rn; i++)
        if (dw[i] > 0) dv[i * 3 + c] = std::clamp(chs[i] / std::max(1e-6f, wsum[i]), -25.f, 25.f);
    }
  }
  {
    struct Lv { int w, h; std::vector<float> v, wt; };
    std::vector<Lv> pyr(1);
    pyr[0] = {rw, rh, dv, dw};
    while (pyr.back().w > 1 || pyr.back().h > 1) {  // push: weighted averages, coarser and coarser
      const Lv& a = pyr.back();
      Lv b{std::max(1, (a.w + 1) / 2), std::max(1, (a.h + 1) / 2), {}, {}};
      b.v.assign((size_t)b.w * b.h * 3, 0.f);
      b.wt.assign((size_t)b.w * b.h, 0.f);
      for (int y = 0; y < a.h; y++)
        for (int x = 0; x < a.w; x++) {
          size_t i = (size_t)y * a.w + x, j = (size_t)(y / 2) * b.w + x / 2;
          b.wt[j] += a.wt[i];
          for (int c = 0; c < 3; c++) b.v[j * 3 + c] += a.v[i * 3 + c] * a.wt[i];
        }
      for (size_t j = 0; j < b.wt.size(); j++)
        if (b.wt[j] > 0)
          for (int c = 0; c < 3; c++) b.v[j * 3 + c] /= b.wt[j];
      for (float& w8 : b.wt) w8 = std::min(w8, 1.f);
      pyr.push_back(std::move(b));
    }
    for (int l = (int)pyr.size() - 2; l >= 0; l--) {  // pull: fill unknown pixels from the coarser level
      Lv& a = pyr[l];
      const Lv& b = pyr[l + 1];
      for (int y = 0; y < a.h; y++)
        for (int x = 0; x < a.w; x++) {
          size_t i = (size_t)y * a.w + x;
          if (a.wt[i] >= 1.f) continue;
          float fx = std::clamp((x + 0.5f) / 2 - 0.5f, 0.f, (float)b.w - 1), fy = std::clamp((y + 0.5f) / 2 - 0.5f, 0.f, (float)b.h - 1);
          int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, b.w - 1), y1 = std::min(y0 + 1, b.h - 1);
          float tx = fx - x0, ty = fy - y0;
          for (int c = 0; c < 3; c++) {
            float up = (b.v[((size_t)y0 * b.w + x0) * 3 + c] * (1 - tx) + b.v[((size_t)y0 * b.w + x1) * 3 + c] * tx) * (1 - ty) +
                       (b.v[((size_t)y1 * b.w + x0) * 3 + c] * (1 - tx) + b.v[((size_t)y1 * b.w + x1) * 3 + c] * tx) * ty;
            a.v[i * 3 + c] = a.v[i * 3 + c] * a.wt[i] + up * (1 - a.wt[i]);
          }
        }
    }
    dv.swap(pyr[0].v);
  }
  for (size_t i = 0; i < rn * 3; i++) f[i] += std::clamp(dv[i], -60.f, 60.f);

  // 2) Grain: an upscaled fill is smoother than the photo around it. Match the fine noise of
  // the ring (luminance high-pass) with deterministic noise of the same strength.
  auto luma = [](const std::vector<float>& v, size_t n) {
    std::vector<float> y(n);
    for (size_t i = 0; i < n; i++) y[i] = 0.299f * v[i * 3] + 0.587f * v[i * 3 + 1] + 0.114f * v[i * 3 + 2];
    return y;
  };
  std::vector<float> oy = luma(o, rn), fy = luma(f, rn), ob, fb;
  ai::boxMean(oy, rw, rh, 1, ob);
  ai::boxMean(fy, rw, rh, 1, fb);
  double so = 0, sf = 0, cnt = 0;
  std::vector<float> ring2;
  ai::boxMean(m, rw, rh, std::max(4, (int)(r * 0.4f)), ring2);
  for (size_t i = 0; i < rn; i++) {
    if (m[i] == 0.f && ring2[i] > 0.f) {
      so += (oy[i] - ob[i]) * (oy[i] - ob[i]);
      cnt += 1;
    }
    if (m[i] > 0.f) sf += (fy[i] - fb[i]) * (fy[i] - fb[i]);
  }
  double mcount = 0;
  for (float v : m) mcount += v > 0.f;
  float sigO = cnt > 0 ? (float)std::sqrt(so / cnt) : 0.f, sigF = mcount > 0 ? (float)std::sqrt(sf / mcount) : 0.f;
  float add = sigO > sigF ? std::sqrt(sigO * sigO - sigF * sigF) * 0.9f : 0.f;
  if (add > 0.3f) {
    uint32_t seed = 0x9e3779b9u ^ (uint32_t)(rx0 * 73856093) ^ (uint32_t)(ry0 * 19349663);
    for (size_t i = 0; i < rn; i++) {
      if (m[i] == 0.f) continue;
      float g = 0;  // ~Gaussian: sum of four uniforms
      for (int k = 0; k < 4; k++) {
        seed = seed * 1664525u + 1013904223u;
        g += (seed >> 8) * (1.f / 16777216.f) - 0.5f;
      }
      g *= add * 1.732f;
      for (int c = 0; c < 3; c++) f[i * 3 + c] += g;
    }
  }

  // 3) Edge: the grown mask is fully replaced (no ghost of the old object at its rim); the fade
  // into the photo happens just outside it, over background that LaMa reproduced.
  int fr = std::max(1, (int)(r * 0.08f));
  std::vector<float> soft;
  ai::boxMean(m, rw, rh, fr, soft);
  out.x = rx0;
  out.y = ry0;
  out.w = rw;
  out.h = rh;
  out.rgba.assign(rn * 4, 0);
  for (size_t i = 0; i < rn; i++) {
    float a = m[i] > 0.f ? 1.f : std::clamp(soft[i] * 2.f, 0.f, 1.f);
    for (int c = 0; c < 3; c++) out.rgba[i * 4 + c] = (uint8_t)std::clamp((int)std::lround(f[i * 3 + c]), 0, 255);
    out.rgba[i * 4 + 3] = (uint8_t)std::lround(a * 255.f);
  }
  return true;
}

static void applyPatch(ai::SmallImage& img, const HealPatch& p) {
  for (int y = 0; y < p.h; y++)
    for (int x = 0; x < p.w; x++) {
      int gx = p.x + x, gy = p.y + y;
      if (gx < 0 || gy < 0 || gx >= img.w || gy >= img.h) continue;
      const uint8_t* s = &p.rgba[((size_t)y * p.w + x) * 4];
      uint8_t* d = &img.rgb[((size_t)gy * img.w + gx) * 3];
      int a = s[3];
      for (int c = 0; c < 3; c++) d[c] = (uint8_t)((s[c] * a + d[c] * (255 - a) + 127) / 255);
    }
}

HealStore::~HealStore() { join(); }

void HealStore::join() {
  if (th_.joinable()) th_.join();
  running_ = false;
}

void HealStore::clear(Renderer& R) {
  join();
  strokes_.clear();
  patches_.clear();
  jobK_ = failedK_ = -1;
  err_.clear();
  R.truncateHeal(0);
}

std::string HealStore::takeError() {
  std::string e;
  e.swap(err_);
  return e;
}

void HealStore::add(EditParams& p, Renderer& R, HealStroke s) {
  int keep = std::clamp(p.heals, 0, (int)strokes_.size());
  if (keep >= kMaxHeals || !cleanStroke(s.pts, s.radius, 0.f, 1.f, 0.5f, kMaxStrokePoints)) return;
  if ((int)strokes_.size() > keep) {
    strokes_.resize(keep);
    patches_.resize(keep);
    R.truncateHeal(keep);
  }
  strokes_.push_back(std::move(s));
  patches_.emplace_back();
  p.heals = (int)strokes_.size();
  failedK_ = -1;
}

bool HealStore::busy(const EditParams& p) const {
  int n = std::min(p.heals, (int)patches_.size());
  for (int k = 0; k < n; k++)
    if (!patches_[k].ready) return k != failedK_;
  return false;
}

bool HealStore::sync(const EditParams& p, Renderer& R, const std::string& photo, bool wait) {
  bool changed = false;
  int W = R.orientedW(), H = R.orientedH();
  if (W <= 0 || H <= 0) return false;
  for (;;) {
    if (running_ && (done_ || wait)) {
      join();
      if (jobK_ >= 0 && jobK_ < (int)patches_.size() && patches_[jobK_].hash == jobOut_.hash) {
        if (jobOut_.ready) {
          patches_[jobK_] = std::move(jobOut_);
        } else {
          failedK_ = jobK_;
          err_ = jobErr_;
        }
      }
      jobOut_ = HealPatch();
      jobK_ = -1;
    }
    int n = std::min(p.heals, (int)strokes_.size());
    int next = -1;
    uint64_t prev = 0;
    for (int k = 0; k < n; k++) {
      HealPatch& P = patches_[k];
      if (!P.hash) P.hash = strokeHash(prev, strokes_[k], W, H, photo);
      prev = P.hash;
      if (P.ready && !P.uploaded) {
        float u0 = (float)P.x / W, v0 = (float)P.y / H;
        R.setHealPatch(k, P.rgba.data(), P.w, P.h, u0, v0, u0 + (float)P.w / W, v0 + (float)P.h / H);
        P.uploaded = true;
        changed = true;
      }
      if (!P.ready && next < 0) next = k;
    }
    if (next < 0 || running_ || next == failedK_) return changed;
    // Start the next patch: from the cache when it is there, else LaMa on the photo with
    // every earlier patch applied (so a second stroke sees the first one's fill).
    std::vector<HealPatch> before(patches_.begin(), patches_.begin() + next);
    HealStroke st = strokes_[next];
    jobK_ = next;
    jobOut_ = HealPatch();
    jobOut_.hash = patches_[next].hash;
    done_ = false;
    running_ = true;
    int maxDim = R.maxTextureSize();
    th_ = std::thread([this, before = std::move(before), st = std::move(st), photo, W, H, maxDim]() {
      try {
        work(before, st, photo, W, H, maxDim);
      } catch (const std::exception& e) {
        jobOut_.ready = false;
        jobErr_ = e.what();
      }
      done_ = true;
      if (g_layerWake) g_layerWake();
    });
    if (!wait) return changed;
  }
}

void HealStore::work(const std::vector<HealPatch>& before, const HealStroke& st, const std::string& photo, int W,
                     int H, int maxDim) {
  {
      HealPatch& out = jobOut_;
      std::string cp = patchPath(photo, out.hash), err;
      int rx0, ry0, rx1, ry1;
      strokeRect(st, W, H, rx0, ry0, rx1, ry1);
      std::vector<uint8_t> rgba;
      int cw = 0, chh = 0;
      if (loadRgbaFile(cp, rgba, cw, chh) && cw == rx1 - rx0 && chh == ry1 - ry0) {
        out.x = rx0;
        out.y = ry0;
        out.w = cw;
        out.h = chh;
        out.rgba.swap(rgba);
        out.ready = true;
      } else {
        ai::SmallImage img;
        ai::Inpainter lama;
        if (!loadLayerPixels(photo, maxDim, img.rgb, img.w, img.h, err)) {
          jobErr_ = err;
        } else if (img.w != W || img.h != H) {
          jobErr_ = "photo size changed";
        } else if (!lama.load(err)) {
          jobErr_ = err;
        } else {
          for (const HealPatch& b : before) applyPatch(img, b);
          if (makePatch(img, st, lama, out, err)) {
            out.ready = true;
            std::string dir = dirOf(photo) + "\\.photoeditor_cache";
            ensureDir(dir);
            setHiddenU(dir);
            std::string werr;
            writePngRgba(cp, out.rgba.data(), out.w, out.h, werr);
          } else {
            jobErr_ = err;
          }
        }
        lama.release();
        ai::ortUnload();
      }
  }
}

std::string HealStore::serialize(const EditParams& p) const {
  std::string out;
  char buf[48];
  int n = std::min(p.heals, (int)strokes_.size());
  for (int k = 0; k < n; k++) {
    const HealStroke& s = strokes_[k];
    snprintf(buf, sizeof(buf), "heal=%.6g", s.radius);
    out += buf;
    for (size_t i = 0; i + 1 < s.pts.size(); i += 2) {
      snprintf(buf, sizeof(buf), ";%.5f,%.5f", s.pts[i], s.pts[i + 1]);
      out += buf;
    }
    out += "\n";
  }
  return out;
}

void HealStore::parse(const std::string& text) {
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.compare(0, 5, "heal=") != 0) continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    HealStroke s;
    s.radius = 0;
    std::stringstream ps(line.substr(5));
    std::string part;
    bool head = true;
    while (std::getline(ps, part, ';')) {
      if (head) {
        s.radius = strtof(part.c_str(), nullptr);
        head = false;
        continue;
      }
      float x, y;
      if (sscanf(part.c_str(), "%f,%f", &x, &y) == 2) {
        s.pts.push_back(x);
        s.pts.push_back(y);
      }
    }
    // Each stroke costs a LaMa run and a rasterised mask at full size: keep them bounded.
    if ((int)strokes_.size() < kMaxHeals && cleanStroke(s.pts, s.radius, 0.f, 1.f, 0.5f, kMaxStrokePoints) &&
        strokeCost(s.pts, s.radius, 0.3f) <= kMaxStrokeCost) {
      strokes_.push_back(std::move(s));
      patches_.emplace_back();
    }
  }
}
