#include "ai/style_match.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "platform_win.h"

namespace ai {

// ---------------------------------------------------------------- StyleSet storage
static const char kStyleMagic[4] = {'P', 'E', 'S', 'T'};

std::string stylesDir() { return exeDir() + "\\styles"; }
std::vector<std::string> listStyleFiles() { return listFiles(stylesDir(), ".lrs"); }

bool StyleSet::save(const std::string& file) const {
  ensureDir(dirOf(file));
  FILE* f = ufopen(file, "wb");
  if (!f) return false;
  uint32_t n = (uint32_t)refs.size(), nl = (uint32_t)name.size();
  bool ok = fwrite(kStyleMagic, 1, 4, f) == 4 && fwrite(&nl, 4, 1, f) == 1 && fwrite(name.data(), 1, nl, f) == nl &&
            fwrite(&n, 4, 1, f) == 1;
  for (const StyleRef& r : refs) {
    uint32_t pl = (uint32_t)r.path.size();
    uint8_t ai = r.hasAi;
    ok = ok && fwrite(&pl, 4, 1, f) == 1 && fwrite(r.path.data(), 1, pl, f) == pl && fwrite(&ai, 1, 1, f) == 1 &&
         fwrite(r.emb, sizeof(r.emb), 1, f) == 1 && fwrite(&r.tone, sizeof(r.tone), 1, f) == 1;
  }
  return (fclose(f) == 0) && ok;
}

bool StyleSet::load(const std::string& file) {
  std::vector<unsigned char> b;
  if (!readFileBytes(file, b) || b.size() < 12 || std::memcmp(b.data(), kStyleMagic, 4) != 0) return false;
  size_t o = 4;
  auto rd = [&](void* d, size_t n) {
    if (o + n > b.size()) return false;
    std::memcpy(d, &b[o], n);
    o += n;
    return true;
  };
  uint32_t nl = 0, n = 0;
  if (!rd(&nl, 4) || nl > 1024 || o + nl > b.size()) return false;
  name.assign((const char*)&b[o], nl);
  o += nl;
  if (!rd(&n, 4) || n > 10000) return false;
  refs.clear();
  for (uint32_t i = 0; i < n; i++) {
    StyleRef r;
    uint32_t pl = 0;
    uint8_t ai = 0;
    if (!rd(&pl, 4) || pl > 4096 || o + pl > b.size()) return false;
    r.path.assign((const char*)&b[o], pl);
    o += pl;
    if (!rd(&ai, 1) || !rd(r.emb, sizeof(r.emb)) || !rd(&r.tone, sizeof(r.tone))) return false;
    r.hasAi = ai != 0;
    refs.push_back(r);
  }
  return true;
}

StyleSet StyleSet::fromPhotos(const std::string& name, const std::vector<PhotoInfo>& photos) {
  StyleSet s;
  s.name = name;
  for (const PhotoInfo& p : photos) {
    StyleRef r;
    r.path = p.path;
    r.hasAi = p.hasAi;
    std::memcpy(r.emb, p.emb, sizeof(r.emb));
    r.tone = p.tone;
    s.refs.push_back(r);
  }
  return s;
}

// ---------------------------------------------------------------- solver
namespace {
struct Var {
  float* v;
  float lo, hi;
  float reg = 1.f;  // relative pull towards neutral
};

struct Ctx {
  Renderer* r;
  EditParams p;
  ToneSig target;
  int w, h;
  std::vector<uint8_t> rgba;
  ToneSig cur;
  float gradeUV[2][2] = {{0, 0}, {0, 0}};  // shadows / highlights as (a, b) offsets
  std::vector<Var> active;
  int evals = 0;
  float regW = 0.004f;

  void applyGrade() {
    for (int z = 0; z < 2; z++) {
      GradeWheel& g = p.grade[z == 0 ? GRADE_SHADOWS : GRADE_HIGHLIGHTS];
      float u = gradeUV[z][0], v = gradeUV[z][1];
      g.sat = std::min(100.f, std::sqrt(u * u + v * v));
      float hue = std::atan2(v, u) * 57.29578f;
      g.hue = g.sat > 0.01f ? (hue < 0 ? hue + 360.f : hue) : 0.f;
    }
  }
  float loss() {
    applyGrade();
    r->renderSmall(p, w, h, rgba);
    toneSignature(rgba.data(), w, h, 4, cur);
    evals++;
    float d = styleDistance(cur, target);
    float reg = 0;
    for (const Var& a : active) {
      float t = *a.v / std::max(std::fabs(a.lo), std::fabs(a.hi));
      reg += a.reg * t * t;
    }
    return d * d + regW * reg;  // pull towards neutral: avoids extreme sliders
  }
};
}  // namespace

static void descend(Ctx& c, std::vector<Var> vars, int passes, float& best) {
  c.active.insert(c.active.end(), vars.begin(), vars.end());
  best = c.loss();
  for (int pass = 0; pass < passes; pass++) {
    for (Var& a : vars) {
      float step = 0.25f * (a.hi - a.lo) / (pass + 1);
      for (int it = 0; it < 7 && step > 0.01f * (a.hi - a.lo); it++) {
        float x = *a.v;
        bool moved = false;
        for (float dir : {1.f, -1.f}) {
          float nx = std::clamp(x + dir * step, a.lo, a.hi);
          if (nx == x) continue;
          *a.v = nx;
          float l = c.loss();
          if (l < best) {
            best = l;
            moved = true;
            break;
          }
          *a.v = x;
        }
        if (!moved) step *= 0.5f;
      }
    }
  }
}

StyleResult solveToTarget(Renderer& renderer, const ToneSig& target, const EditParams& base, unsigned groups,
                          const SolveOptions& opt) {
  auto t0 = std::chrono::steady_clock::now();
  StyleResult res;
  Ctx c;
  c.r = &renderer;
  copyGeometry(c.p, base);
  c.target = target;
  c.regW = opt.reg;
  Geometry g = computeGeometry(c.p, renderer.srcW(), renderer.srcH(), renderer.orientation(), false);
  float s = 192.f / std::max(g.outW, g.outH);
  c.w = std::max(8, (int)std::lround(g.outW * s));
  c.h = std::max(8, (int)std::lround(g.outH * s));

  float best = c.loss();
  res.lossBefore = std::sqrt(best);
  EditParams& p = c.p;
  if (groups & SG_LIGHT) {
    descend(c, {{&p.exposure, -2.f, 2.f}, {&p.contrast, -60, 60}, {&p.highlights, -80, 80}, {&p.shadows, -80, 80},
                {&p.whites, -60, 60}, {&p.blacks, -60, 60}},
            2, best);
  }
  if (groups & SG_COLOR) {
    descend(c, {{&p.temp, -60, 60}, {&p.tint, -40, 40}, {&p.vibrance, opt.minSat, 60}, {&p.saturation, opt.minSat, 60}},
            2, best);
  }
  if (groups & SG_HSL) {
    std::vector<Var> v;
    for (int b = 0; b < kHslBands; b++) {
      float tShare = c.target.v[20 + b * 4], cShare = c.cur.v[20 + b * 4];
      if (tShare < 0.02f || cShare < 0.02f) continue;  // colour absent in either image
      v.push_back({&p.hslHue[b], -20, 20, 3.f});
      v.push_back({&p.hslSat[b], -50, 50, 3.f});
      v.push_back({&p.hslLum[b], -40, 40, 3.f});
    }
    if (!v.empty()) descend(c, v, 1, best);
  }
  if (groups & SG_GRADE) {
    descend(c, {{&c.gradeUV[0][0], -40, 40, 2.f}, {&c.gradeUV[0][1], -40, 40, 2.f}, {&c.gradeUV[1][0], -40, 40, 2.f},
                {&c.gradeUV[1][1], -40, 40, 2.f}},
            2, best);
  }
  if (groups & SG_CURVE) {
    // Pull the remaining L* percentile error into a 5-point master curve.
    c.loss();
    EditParams keep = p;
    Curve cv;
    std::memset(&cv, 0, sizeof(cv));
    cv.p[cv.n++] = {0.f, 0.f};
    const int idx[5] = {1, 2, 3, 4, 5};  // p5 p25 p50 p75 p95
    float lastX = 0, lastY = 0;
    for (int i : idx) {
      float x = c.cur.v[i], y = x + 0.6f * (c.target.v[i] - x);
      if (x < lastX + 0.04f || x > 0.96f) continue;
      y = std::clamp(y, lastY + 0.01f, 0.99f);
      cv.p[cv.n++] = {x, y};
      lastX = x;
      lastY = y;
    }
    cv.p[cv.n++] = {1.f, 1.f};
    p.curve[CURVE_MASTER] = cv;
    float l = c.loss();
    if (l < best) best = l;
    else p = keep;
  }
  c.applyGrade();
  res.lossAfter = std::sqrt(best);
  res.params = c.p;
  res.evals = c.evals;
  res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return res;
}

StyleResult solveStyle(Renderer& renderer, const StyleSet& set, const float* emb, const EditParams& base, unsigned groups) {
  auto t0 = std::chrono::steady_clock::now();
  StyleResult res;
  Ctx c;
  c.r = &renderer;
  copyGeometry(c.p, base);
  // Target signature: references weighted by scene similarity (softmax over cosine).
  std::vector<float> wts(set.refs.size(), 1.f);
  if (emb) {
    float mx = -1e9f;
    for (size_t i = 0; i < set.refs.size(); i++) {
      float s = 0;
      if (set.refs[i].hasAi)
        for (int k = 0; k < kEmbDim; k++) s += emb[k] * set.refs[i].emb[k];
      wts[i] = s / 0.05f;
      mx = std::max(mx, wts[i]);
    }
    for (float& x : wts) x = std::exp(x - mx);
  }
  float wsum = 0;
  for (float x : wts) wsum += x;
  for (float x : wts) res.weights.push_back(x / wsum);
  std::memset(&c.target, 0, sizeof(c.target));
  for (size_t i = 0; i < set.refs.size(); i++)
    for (int k = 0; k < kToneDim; k++) c.target.v[k] += set.refs[i].tone.v[k] * wts[i] / wsum;

  StyleResult r2 = solveToTarget(renderer, c.target, base, groups);
  r2.weights = res.weights;
  r2.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return r2;
}

EditParams blendStyle(const EditParams& base, const EditParams& opt, float t) {
  EditParams def, out;
  t = std::clamp(t, 0.f, 1.5f);  // > 1 exaggerates (auto-edit candidates)
  auto L = [&](float a, float b) { return a + (b - a) * t; };
  out.temp = L(def.temp, opt.temp); out.tint = L(def.tint, opt.tint); out.exposure = L(def.exposure, opt.exposure);
  out.contrast = L(def.contrast, opt.contrast); out.highlights = L(def.highlights, opt.highlights);
  out.shadows = L(def.shadows, opt.shadows); out.whites = L(def.whites, opt.whites); out.blacks = L(def.blacks, opt.blacks);
  out.texture = L(def.texture, opt.texture); out.clarity = L(def.clarity, opt.clarity); out.dehaze = L(def.dehaze, opt.dehaze);
  out.vibrance = L(def.vibrance, opt.vibrance); out.saturation = L(def.saturation, opt.saturation);
  for (int b = 0; b < kHslBands; b++) {
    out.hslHue[b] = L(0, opt.hslHue[b]);
    out.hslSat[b] = L(0, opt.hslSat[b]);
    out.hslLum[b] = L(0, opt.hslLum[b]);
  }
  for (int z = 0; z < GRADE_COUNT; z++) {
    out.grade[z].hue = opt.grade[z].hue;
    out.grade[z].sat = L(0, opt.grade[z].sat);
    out.grade[z].lum = L(0, opt.grade[z].lum);
  }
  out.gradeBlending = opt.gradeBlending;
  out.gradeBalance = L(def.gradeBalance, opt.gradeBalance);
  for (int k = 0; k < CURVE_COUNT; k++) {  // curves: blend each point towards the diagonal
    out.curve[k] = opt.curve[k];
    for (int i = 0; i < out.curve[k].n; i++) {
      CurvePt& q = out.curve[k].p[i];
      q.y = q.x + (q.y - q.x) * t;
    }
  }
  out.sharpAmount = opt.sharpAmount; out.sharpRadius = opt.sharpRadius; out.sharpMasking = opt.sharpMasking;
  out.nrLum = opt.nrLum; out.nrColor = opt.nrColor;
  out.vigAmount = L(def.vigAmount, opt.vigAmount); out.vigMidpoint = opt.vigMidpoint; out.vigFeather = opt.vigFeather;
  out.vigRoundness = opt.vigRoundness;
  out.grainAmount = L(def.grainAmount, opt.grainAmount); out.grainSize = opt.grainSize;
  out.grainRoughness = opt.grainRoughness;
  copyGeometry(out, base);
  copyLayers(out, base);
  return out;
}

}  // namespace ai
