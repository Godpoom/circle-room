#include "ai/auto_edit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace ai {

namespace {
struct Profile {
  float mid;        // target median L* (0..1)
  float p1, p99;    // target black / white points
  float chroma;     // chroma multiplier
  float local;      // local-contrast multiplier
  float warmKeep;   // share of a warm cast that is kept (sunsets, candle light)
};

const Profile kProfiles[SC_COUNT + 1] = {
    {0.55f, 0.03f, 0.96f, 1.08f, 1.00f, 0.3f},  // portrait
    {0.53f, 0.03f, 0.96f, 1.10f, 1.05f, 0.3f},  // group
    {0.50f, 0.03f, 0.96f, 1.20f, 1.25f, 0.0f},  // landscape
    {0.45f, 0.03f, 0.97f, 1.20f, 1.15f, 1.0f},  // sunset
    {0.30f, 0.02f, 0.95f, 1.10f, 1.10f, 0.6f},  // night
    {0.50f, 0.03f, 0.95f, 1.08f, 1.05f, 0.5f},  // indoor
    {0.55f, 0.03f, 0.96f, 1.20f, 1.10f, 0.6f},  // food
    {0.58f, 0.03f, 0.97f, 1.18f, 1.10f, 0.2f},  // beach
    {0.65f, 0.03f, 0.98f, 1.05f, 1.10f, 0.0f},  // snow
    {0.48f, 0.03f, 0.96f, 1.12f, 1.20f, 0.0f},  // forest
    {0.50f, 0.03f, 0.96f, 1.08f, 1.25f, 0.2f},  // city
    {0.50f, 0.03f, 0.96f, 1.10f, 1.15f, 0.2f},  // animal
    {0.50f, 0.03f, 0.96f, 1.15f, 1.15f, 0.2f},  // generic (no scene)
};

float regionMean(const SegResult& seg, const std::vector<float>& guide, int gw, int gh, int region, float& area) {
  area = seg.area[region];
  if (area < 0.02f || guide.empty()) return -1.f;
  if (gw <= 0 || gh <= 0 || guide.size() != (size_t)gw * gh) return -1.f;
  double s = 0, w = 0;
  const uint8_t* pr = &seg.prob[(size_t)region * seg.w * seg.h];
  for (int y = 0; y < seg.h; y++)
    for (int x = 0; x < seg.w; x++) {
      float p = pr[y * seg.w + x] / 255.f;
      if (p < 0.5f) continue;
      int gx = std::min(gw - 1, x * gw / seg.w), gy = std::min(gh - 1, y * gh / seg.h);
      s += p * guide[(size_t)gy * gw + gx];
      w += p;
    }
  return w > 0 ? (float)(s / w) : -1.f;
}
}  // namespace

static ToneSig buildTarget(const ToneSig& cur, const Profile& pf) {
  ToneSig t = cur;
  const float* q = cur.v;  // L percentiles 1,5,25,50,75,95,99
  float mid = q[3] + 0.6f * (pf.mid - q[3]);  // only partly: keeps intentionally dark/bright photos
  auto remap = [&](float x) {
    float lo = q[0], md = std::max(q[3], lo + 0.01f), hi = std::max(q[6], md + 0.01f);
    float y = x <= md ? pf.p1 + (x - lo) / (md - lo) * (mid - pf.p1) : mid + (x - md) / (hi - md) * (pf.p99 - mid);
    return std::clamp(y, 0.f, 1.f);
  };
  for (int i = 0; i <= 6; i++) t.v[i] = q[i] + 0.85f * (remap(q[i]) - q[i]);
  float spread = (t.v[6] - t.v[0]) / std::max(0.05f, q[6] - q[0]);
  t.v[7] = cur.v[7] * std::clamp(spread, 0.7f, 1.6f);
  t.v[8] = cur.v[8] * pf.local;
  // Auto white balance on near-neutral pixels; keep part of a warm cast where it belongs.
  double dA = 0, dB = 0;
  for (int z = 0; z < 3; z++) {
    float a = cur.v[11 + z * 2], b = cur.v[12 + z * 2];
    float ka = 0.2f, kb = b > 0 ? std::max(0.2f, pf.warmKeep) : 0.2f;
    t.v[11 + z * 2] = a * ka;
    t.v[12 + z * 2] = b * kb;
    dA += a - a * ka;
    dB += b - b * kb;
  }
  t.v[9] = cur.v[9] - (float)(dA / 3);
  t.v[10] = cur.v[10] - (float)(dB / 3);
  // Lab chroma grows with lightness: brightening a dark photo must not read as "too colourful".
  float lightK = std::clamp(std::pow(t.v[3] / std::max(0.05f, q[3]), 0.7f), 0.8f, 2.0f);
  for (int i = 17; i <= 19; i++) t.v[i] = std::min(0.9f, cur.v[i] * pf.chroma * lightK);
  for (int B = 0; B < 8; B++) t.v[20 + B * 4 + 1] = std::min(0.9f, cur.v[20 + B * 4 + 1] * pf.chroma * lightK);
  return t;
}

static void addAutoLayers(const AutoInputs& in, int scene, EditParams& p) {
  if (!in.seg || !in.seg->valid() || !in.guide || anyLayer(p)) return;  // never override the user's layers
  int slot = 0;
  auto add = [&](int region, bool invert) -> LocalLayer* {
    if (slot >= kMaxLayers) return nullptr;
    LocalLayer& l = p.layers[slot++];
    std::memset((void*)&l, 0, sizeof(l));
    l.enabled = 1;
    l.region = region;
    l.invert = invert;
    l.feather = 15.f;
    return &l;
  };
  float area;
  float sky = regionMean(*in.seg, *in.guide, in.guideW, in.guideH, REG_SKY, area);
  if (sky >= 0 && area > 0.05f) {
    LocalLayer* l = add(REG_SKY, false);
    l->highlights = sky > 0.75f ? -35.f : -15.f;  // bring back bright skies
    l->saturation = sky > 0.75f ? 15.f : 10.f;
    l->contrast = 8.f;
  }
  float person = regionMean(*in.seg, *in.guide, in.guideW, in.guideH, REG_PERSON, area);
  if (person >= 0 && area > 0.02f) {
    float lift = std::clamp((0.5f - person) * 1.6f, 0.f, 0.8f);  // darker than mid grey -> lift
    if (lift > 0.05f) {
      LocalLayer* l = add(REG_PERSON, false);
      l->exposure = std::round(lift * 100.f) / 100.f;
      l->shadows = 15.f;
    }
    if ((scene == SC_PORTRAIT || scene == SC_GROUP) && area > 0.08f) {  // subject pops slightly
      LocalLayer* l = add(REG_PERSON, true);
      if (l) {
        l->exposure = -0.12f;
        l->saturation = -5.f;
      }
    }
  }
}

AutoResult autoEdit(Renderer& r, const AutoInputs& in, const EditParams& base) {
  auto t0 = std::chrono::steady_clock::now();
  AutoResult out;
  int scene = -1;
  if (in.emb && in.clip) scene = in.clip->scene(in.emb, &out.sceneConf);
  if (out.sceneConf < 0.25f) scene = -1;  // unsure -> generic profile
  out.scene = scene;
  const Profile& pf = kProfiles[scene < 0 ? SC_COUNT : scene];

  // Current signature of the photo with no edits (geometry kept).
  EditParams plain;
  copyGeometry(plain, base);
  Geometry g = computeGeometry(plain, r.srcW(), r.srcH(), r.orientation(), false);
  float k = 192.f / std::max(g.outW, g.outH);
  int w = std::max(8, (int)std::lround(g.outW * k)), h = std::max(8, (int)std::lround(g.outH * k));
  std::vector<uint8_t> rgba;
  r.renderSmall(plain, w, h, rgba);
  ToneSig cur;
  toneSignature(rgba.data(), w, h, 4, cur);
  SolveOptions so;
  so.reg = 0.012f;    // auto edits should stay moderate and easy to tweak
  so.minSat = -15.f;  // never wash colours out on its own
  StyleResult sol = solveToTarget(r, buildTarget(cur, pf), base, SG_LIGHT | SG_COLOR | SG_CURVE, so);
  EditParams best = blendStyle(base, sol.params, 1.f);
  addAutoLayers(in, scene, best);

  // 9 variants (strength x warmth); CLIP aesthetic + exposure/lighting prompts pick one.
  if (in.clip && in.clip->ok()) {
    const float strengths[3] = {0.7f, 1.f, 1.2f}, warmth[3] = {-8.f, 0.f, 8.f};
    std::vector<EditParams> cands;
    std::vector<float> tensors((size_t)9 * kClipTensor), clipPenalty(9, 0.f);
    float ck = 224.f / std::min(g.outW, g.outH);
    int cw = std::max(224, (int)std::lround(g.outW * ck)), chh = std::max(224, (int)std::lround(g.outH * ck));
    for (int i = 0; i < 9; i++) {
      EditParams c = blendStyle(base, sol.params, strengths[i / 3]);
      c.temp = std::clamp(c.temp + warmth[i % 3], -100.f, 100.f);
      copyLayers(c, best);
      cands.push_back(c);
      r.renderSmall(c, cw, chh, rgba);
      SmallImage si;
      si.w = cw;
      si.h = chh;
      si.rgb.resize((size_t)cw * chh * 3);
      size_t hi = 0, lo = 0;
      for (size_t px = 0; px < (size_t)cw * chh; px++) {
        for (int ch = 0; ch < 3; ch++) si.rgb[px * 3 + ch] = rgba[px * 4 + ch];
        int y = (rgba[px * 4] * 54 + rgba[px * 4 + 1] * 183 + rgba[px * 4 + 2] * 19) >> 8;
        if (y >= 252) hi++;
        if (y <= 3) lo++;
      }
      clipPenalty[i] = 4.f * (float)(hi + lo) / (cw * chh);
      Clip::prep(si, &tensors[(size_t)i * kClipTensor]);
    }
    std::vector<float> emb((size_t)9 * kEmbDim);
    std::string err;
    if (in.clip->embed(tensors.data(), 9, emb.data(), err)) {
      float bestScore = -1e9f;
      for (int i = 0; i < 9; i++) {
        const float* e = &emb[(size_t)i * kEmbDim];
        float s = in.clip->aesthetic(e) + 1.5f * (in.clip->prompt(e, P_EXPOSURE) + in.clip->prompt(e, P_LIGHTING)) -
                  clipPenalty[i] - (i == 4 ? 0.f : 0.05f);  // tiny preference for the plain solution
        if (s > bestScore) {
          bestScore = s;
          out.candidate = i;
        }
      }
      out.score = bestScore;
      best = cands[out.candidate];
    }
  }
  out.params = best;
  out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return out;
}

}  // namespace ai
