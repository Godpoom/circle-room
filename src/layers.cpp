#include "layers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "ai/wic_decode.h"
#include "gl_loader.h"
#include "image_io.h"
#include "renderer.h"

void (*g_layerWake)() = nullptr;

static void wake() {
  if (g_layerWake) g_layerWake();
}

static float srgbToLinear(uint8_t v) {
  float c = v / 255.f;
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bool loadLayerPixels(const std::string& path, int maxDim, std::vector<uint8_t>& rgb, int& w, int& h, std::string& err) {
  ImageData d;
  if (!loadImage(path, maxDim, d, err)) return false;
  std::vector<uint8_t> px;
  if (d.is16) {  // linear 16-bit -> sRGB bytes
    static uint8_t lut[4096];
    static bool init = false;
    if (!init) {
      for (int i = 0; i < 4096; i++) {
        double l = (i + 0.5) / 4096.0;
        lut[i] = (uint8_t)std::lround((l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055) * 255.0);
      }
      init = true;
    }
    px.resize(d.px16.size());
    for (size_t i = 0; i < px.size(); i++) px[i] = lut[d.px16[i] >> 4];
  } else {
    px.swap(d.px8);
  }
  int W = d.w, H = d.h, o = d.orientation;
  d.release();
  w = o >= 5 ? H : W;
  h = o >= 5 ? W : H;
  if (o <= 1) {
    rgb.swap(px);
    return true;
  }
  rgb.resize((size_t)w * h * 3);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int sx = x, sy = y;
      switch (o) {
        case 2: sx = W - 1 - x; break;
        case 3: sx = W - 1 - x; sy = H - 1 - y; break;
        case 4: sy = H - 1 - y; break;
        case 5: sx = y; sy = x; break;
        case 6: sx = y; sy = H - 1 - x; break;
        case 7: sx = W - 1 - y; sy = H - 1 - x; break;
        case 8: sx = W - 1 - y; sy = x; break;
      }
      std::memcpy(&rgb[((size_t)y * w + x) * 3], &px[((size_t)sy * W + sx) * 3], 3);
    }
  return true;
}

static void makeThumb(const std::vector<uint8_t>& rgb, int w, int h, int maxDim, std::vector<uint8_t>& out, int& tw,
                      int& th) {
  float k = std::min(1.f, (float)maxDim / std::max(w, h));
  tw = std::max(1, (int)(w * k));
  th = std::max(1, (int)(h * k));
  out.assign((size_t)tw * th * 3, 0);
  for (int y = 0; y < th; y++) {
    int y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
    for (int x = 0; x < tw; x++) {
      int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw);
      unsigned s[3] = {}, n = 0;
      for (int yy = y0; yy < y1; yy += std::max(1, (y1 - y0) / 4))
        for (int xx = x0; xx < x1; xx += std::max(1, (x1 - x0) / 4), n++)
          for (int c = 0; c < 3; c++) s[c] += rgb[((size_t)yy * w + xx) * 3 + c];
      for (int c = 0; c < 3; c++) out[((size_t)y * tw + x) * 3 + c] = (uint8_t)(s[c] / std::max(1u, n));
    }
  }
}

LayerAsset* LayerStore::asset(int id) {
  auto it = assets_.find(id);
  return it == assets_.end() ? nullptr : it->second.get();
}

void LayerStore::clear(Renderer& R) {
  for (auto& kv : assets_) {
    if (kv.second->thumbTex) glDeleteTextures(1, &kv.second->thumbTex);
  }
  assets_.clear();  // joins any worker still running
  strokes_.clear();
  gen_.clear();
  R.dropAllLayers();
}

int LayerStore::nextId(const EditParams& p) const {
  int id = 0;
  for (const ImageLayer& l : p.img) id = std::max(id, l.id);
  for (const auto& kv : assets_) id = std::max(id, kv.first);
  for (const auto& kv : strokes_) id = std::max(id, kv.first);
  return id + 1;
}

bool LayerStore::busy(const EditParams& p) {
  for (const ImageLayer& l : p.img) {
    if (!l.enabled) continue;
    LayerAsset* a = asset(l.id);
    if (!a || (a->stage != 4 && a->stage != -1)) return true;
  }
  return false;
}

// One soft round dab: erase pulls alpha down to 1 - f, restore pushes it up to f.
static void dab(std::vector<float>& a, int W, int H, float cx, float cy, float r, float hard, bool erase) {
  int x0 = std::max(0, (int)(cx - r)), x1 = std::min(W - 1, (int)(cx + r) + 1);
  int y0 = std::max(0, (int)(cy - r)), y1 = std::min(H - 1, (int)(cy + r) + 1);
  float inner = std::clamp(hard, 0.f, 0.98f);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy) / r;
      if (d >= 1.f) continue;
      float t = d <= inner ? 1.f : (1.f - d) / (1.f - inner);
      float f = t * t * (3 - 2 * t);
      float& v = a[(size_t)y * W + x];
      v = erase ? std::min(v, 1.f - f) : std::max(v, f);
    }
}

// Dabs for points [from, n) of the stroke (each new point continues the line from the previous).
static void applyStroke(std::vector<float>& a, int W, int H, const Stroke& s, size_t from) {
  size_t n = s.pts.size() / 2;
  float r = std::max(0.75f, s.radius * W), step = std::max(0.75f, r * 0.25f);
  for (size_t k = from; k < n; k++) {
    float x = s.pts[k * 2] * W, y = s.pts[k * 2 + 1] * H;
    if (k == 0) {
      dab(a, W, H, x, y, r, s.hard, s.erase != 0);
      continue;
    }
    float px = s.pts[k * 2 - 2] * W, py = s.pts[k * 2 - 1] * H;
    float len = std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
    int steps = std::max(1, (int)std::ceil(len / step));
    for (int i = 1; i <= steps; i++) {
      float t = (float)i / steps;
      dab(a, W, H, px + (x - px) * t, py + (y - py) * t, r, s.hard, s.erase != 0);
    }
  }
}

bool LayerStore::sync(const EditParams& p, Renderer& R, bool wait) {
  bool changed = false;
  for (const ImageLayer& l : p.img) {
    if (!l.enabled || !l.path[0]) continue;
    auto& slot = assets_[l.id];
    if (slot && slot->path != l.path) {
      R.dropLayer(l.id);
      if (slot->thumbTex) glDeleteTextures(1, &slot->thumbTex);
      slot.reset();
      changed = true;
    }
    if (!slot) {
      slot.reset(new LayerAsset());
      LayerAsset* A = slot.get();
      A->id = l.id;
      A->path = l.path;
      A->stage = 1;
      int maxDim = std::min(4096, R.maxTextureSize());
      A->th = std::thread([A, maxDim]() {
        if (!loadLayerPixels(A->path, maxDim, A->rgb, A->w, A->h, A->err)) {
          A->stage = -1;
          wake();
          return;
        }
        makeThumb(A->rgb, A->w, A->h, 160, A->thumb, A->tw, A->th2);
        A->stage = 2;
        wake();
        std::string e2;
        A->segOk = ai::segmentFile(A->path, A->seg, A->guide, A->gw, A->gh, e2);
        A->stage = 4;
        wake();
      });
    }
    LayerAsset* A = slot.get();
    if (wait && A->th.joinable()) A->th.join();
    int st = A->stage;
    if (st >= 2 && !A->uploaded) {
      R.setLayerImage(A->id, A->rgb.data(), A->w, A->h);
      std::vector<uint8_t>().swap(A->rgb);
      std::vector<uint8_t> rgba((size_t)A->tw * A->th2 * 4, 255);
      for (size_t i = 0; i < (size_t)A->tw * A->th2; i++) std::memcpy(&rgba[i * 4], &A->thumb[i * 3], 3);
      glGenTextures(1, &A->thumbTex);
      glBindTexture(GL_TEXTURE_2D, A->thumbTex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, A->tw, A->th2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      A->uploaded = true;
      changed = true;
    }
    if (st == 4 && A->th.joinable()) A->th.join();
    if (!A->uploaded) continue;

    // ---- mask: AI cut-out, then the brush strokes in order
    std::vector<Stroke>& S = strokes_[l.id];
    int n = std::min(l.strokes, (int)S.size());
    if (!l.keep && n == 0) {
      if (A->maskOn) {
        R.setLayerMask(A->id, nullptr, 0, 0);
        A->maskOn = false;
        changed = true;
      }
      continue;
    }
    bool segReady = st == 4 && A->segOk;
    int keep = segReady ? l.keep : 0;
    int mw = segReady ? A->gw : 0, mh = segReady ? A->gh : 0;
    if (!segReady) {
      float k = std::min(1.f, 1536.f / std::max(A->w, A->h));
      mw = std::max(1, (int)(A->w * k));
      mh = std::max(1, (int)(A->h * k));
    }
    bool rebuild = keep != A->builtKeep || l.feather != A->builtFeather || mw != A->mw || mh != A->mh;
    if (rebuild) {
      if (keep) ai::buildKeepMask(A->seg, keep, l.feather, A->guide, mw, mh, A->base);
      else A->base.assign((size_t)mw * mh, 1.f);
      A->mw = mw;
      A->mh = mh;
      A->builtKeep = keep;
      A->builtFeather = l.feather;
    }
    int gen = gen_[l.id];
    bool any = rebuild;
    if (rebuild || n < A->doneStrokes || gen != A->builtGen ||
        (n == A->doneStrokes && n > 0 && S[n - 1].pts.size() / 2 < A->donePts)) {
      A->alpha = A->base;
      A->doneStrokes = 0;
      A->donePts = 0;
      A->builtGen = gen;
      any = true;
    }
    for (int k = std::max(0, A->doneStrokes - 1); k < n; k++) {
      size_t from = k == A->doneStrokes - 1 ? A->donePts : 0;
      if (from < S[k].pts.size() / 2) {
        applyStroke(A->alpha, mw, mh, S[k], from);
        any = true;
      }
    }
    A->doneStrokes = n;
    A->donePts = n > 0 ? S[n - 1].pts.size() / 2 : 0;
    if (any || !A->maskOn) {
      std::vector<uint8_t> a8(A->alpha.size());
      for (size_t i = 0; i < a8.size(); i++) a8[i] = (uint8_t)std::lround(std::clamp(A->alpha[i], 0.f, 1.f) * 255.f);
      R.setLayerMask(A->id, a8.data(), mw, mh);
      A->maskOn = true;
      changed = true;
    }
  }
  return changed;
}

void LayerStore::beginStroke(ImageLayer& l, int erase, float radius, float hard, float x, float y) {
  std::vector<Stroke>& S = strokes_[l.id];
  if ((int)S.size() > l.strokes) {
    S.resize(std::max(0, l.strokes));
    gen_[l.id]++;
  }
  Stroke s;
  s.erase = erase;
  s.radius = radius;
  s.hard = hard;
  s.pts = {x, y};
  S.push_back(std::move(s));
  l.strokes = (int)S.size();
}

void LayerStore::extendStroke(const ImageLayer& l, float x, float y) {
  std::vector<Stroke>& S = strokes_[l.id];
  if (S.empty() || (int)S.size() != l.strokes) return;
  std::vector<float>& pts = S.back().pts;
  float dx = x - pts[pts.size() - 2], dy = y - pts[pts.size() - 1];
  if (dx * dx + dy * dy < 1e-8f) return;
  pts.push_back(x);
  pts.push_back(y);
}

std::string LayerStore::serializeStrokes(const EditParams& p) const {
  std::string out;
  char buf[64];
  for (const ImageLayer& l : p.img) {
    if (!l.enabled || l.strokes <= 0) continue;
    auto it = strokes_.find(l.id);
    if (it == strokes_.end()) continue;
    int n = std::min(l.strokes, (int)it->second.size());
    for (int k = 0; k < n; k++) {
      const Stroke& s = it->second[k];
      snprintf(buf, sizeof(buf), "stroke%d=%d,%.5g,%.3g", l.id, s.erase, s.radius, s.hard);
      out += buf;
      for (size_t i = 0; i + 1 < s.pts.size(); i += 2) {
        snprintf(buf, sizeof(buf), ";%.4f,%.4f", s.pts[i], s.pts[i + 1]);
        out += buf;
      }
      out += "\n";
    }
  }
  return out;
}

void LayerStore::parseStrokes(const std::string& text) {
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.compare(0, 6, "stroke") != 0) continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    int id = atoi(line.c_str() + 6);
    Stroke s;
    std::stringstream ps(line.substr(eq + 1));
    std::string part;
    bool head = true;
    while (std::getline(ps, part, ';')) {
      if (head) {
        if (sscanf(part.c_str(), "%d,%f,%f", &s.erase, &s.radius, &s.hard) != 3) break;
        head = false;
        continue;
      }
      float x, y;
      if (sscanf(part.c_str(), "%f,%f", &x, &y) == 2) {
        s.pts.push_back(x);
        s.pts.push_back(y);
      }
    }
    if (!head && !s.pts.empty()) strokes_[id].push_back(std::move(s));
  }
}

bool LayerStore::matchColours(const std::string& basePath, ImageLayer& l) {
  LayerAsset* A = asset(l.id);
  if (!A || A->thumb.empty()) return false;
  ai::SmallImage b;
  std::string err;
  if (!ai::decodeScaled(basePath, 256, b, nullptr, err)) return false;
  auto stats = [](const uint8_t* rgb, size_t n, const float* w, float out[4]) {
    double s[3] = {}, ws = 0, chroma = 0;
    for (size_t i = 0; i < n; i++) {
      float k = w ? w[i] : 1.f;
      if (k < 0.5f) continue;
      float c[3] = {srgbToLinear(rgb[i * 3]), srgbToLinear(rgb[i * 3 + 1]), srgbToLinear(rgb[i * 3 + 2])};
      float Y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
      for (int j = 0; j < 3; j++) s[j] += c[j];
      chroma += std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
      (void)Y;
      ws += 1;
    }
    if (ws < 1) return false;
    for (int j = 0; j < 3; j++) out[j] = (float)(s[j] / ws);
    out[3] = (float)(chroma / ws);
    return true;
  };
  float bs[4], ls[4];
  std::vector<float> w;
  if (A->maskOn && A->mw > 0) {  // only the visible part of the layer counts
    w.resize((size_t)A->tw * A->th2);
    for (int y = 0; y < A->th2; y++)
      for (int x = 0; x < A->tw; x++)
        w[(size_t)y * A->tw + x] = A->alpha[(size_t)(y * A->mh / A->th2) * A->mw + x * A->mw / A->tw];
  }
  if (!stats(b.rgb.data(), (size_t)b.w * b.h, nullptr, bs) ||
      !stats(A->thumb.data(), (size_t)A->tw * A->th2, w.empty() ? nullptr : w.data(), ls))
    return false;
  auto Y = [](const float* c) { return std::max(1e-4f, 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]); };
  l.exposure = std::clamp(0.6f * std::log2(Y(bs) / Y(ls)), -2.f, 2.f);
  float k = std::pow((bs[0] / std::max(1e-4f, bs[2])) / (ls[0] / std::max(1e-4f, ls[2])), 0.6f);
  l.temp = std::clamp(400.f * (k - 1.f) / (k + 1.f), -60.f, 60.f);
  float cs = (bs[3] / Y(bs)) / std::max(1e-4f, ls[3] / Y(ls));
  l.saturation = std::clamp(100.f * (std::sqrt(cs) - 1.f), -40.f, 40.f);
  return true;
}
