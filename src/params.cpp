#include "params.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "platform_win.h"

EditParams::EditParams() {
  std::memset((void*)this, 0, sizeof(*this));
  for (auto& c : curve) {
    c.n = 2;
    c.p[0] = {0.f, 0.f};
    c.p[1] = {1.f, 1.f};
  }
  gradeBlending = 50.f;
  sharpRadius = 1.0f;
  vigMidpoint = 50.f;
  vigFeather = 50.f;
  grainSize = 25.f;
  grainRoughness = 50.f;
  cropW = cropH = 1.f;
}

bool sameParams(const EditParams& a, const EditParams& b) { return std::memcmp(&a, &b, sizeof(EditParams)) == 0; }
bool sameCurves(const EditParams& a, const EditParams& b) { return std::memcmp(a.curve, b.curve, sizeof(a.curve)) == 0; }
bool isDefault(const EditParams& p) { return sameParams(p, EditParams()); }

void copyGeometry(EditParams& dst, const EditParams& src) {
  dst.rotate90 = src.rotate90;
  dst.flipH = src.flipH;
  dst.flipV = src.flipV;
  dst.cropX = src.cropX;
  dst.cropY = src.cropY;
  dst.cropW = src.cropW;
  dst.cropH = src.cropH;
  dst.straighten = src.straighten;
}

void resetGeometry(EditParams& p) { copyGeometry(p, EditParams()); }

void copyLayers(EditParams& dst, const EditParams& src) {
  std::memcpy(dst.layers, src.layers, sizeof(dst.layers));
  dst.recover = src.recover;
  std::memcpy(dst.img, src.img, sizeof(dst.img));
  dst.heals = src.heals;
}

bool anyImageLayer(const EditParams& p) {
  for (const ImageLayer& l : p.img)
    if (l.enabled && !l.hidden) return true;
  return false;
}

bool sameImageLayers(const EditParams& a, const EditParams& b) { return std::memcmp(a.img, b.img, sizeof(a.img)) == 0; }

bool anyLayer(const EditParams& p) {
  for (const LocalLayer& l : p.layers)
    if (l.enabled) return true;
  return false;
}

// Calls f(key, float&) for every scalar; geometry only when requested.
template <class F>
static void visitScalars(EditParams& p, bool geom, F f) {
  f("temp", p.temp); f("tint", p.tint); f("exposure", p.exposure);
  f("contrast", p.contrast); f("highlights", p.highlights); f("shadows", p.shadows);
  f("whites", p.whites); f("blacks", p.blacks);
  f("texture", p.texture); f("clarity", p.clarity); f("dehaze", p.dehaze);
  f("vibrance", p.vibrance); f("saturation", p.saturation);
  char key[32];
  for (int i = 0; i < kHslBands; i++) {
    snprintf(key, sizeof(key), "hslHue%d", i); f(key, p.hslHue[i]);
    snprintf(key, sizeof(key), "hslSat%d", i); f(key, p.hslSat[i]);
    snprintf(key, sizeof(key), "hslLum%d", i); f(key, p.hslLum[i]);
  }
  for (int i = 0; i < GRADE_COUNT; i++) {
    snprintf(key, sizeof(key), "grade%dHue", i); f(key, p.grade[i].hue);
    snprintf(key, sizeof(key), "grade%dSat", i); f(key, p.grade[i].sat);
    snprintf(key, sizeof(key), "grade%dLum", i); f(key, p.grade[i].lum);
  }
  f("gradeBlending", p.gradeBlending); f("gradeBalance", p.gradeBalance);
  f("sharpAmount", p.sharpAmount); f("sharpRadius", p.sharpRadius); f("sharpMasking", p.sharpMasking);
  f("nrLum", p.nrLum); f("nrColor", p.nrColor);
  f("vigAmount", p.vigAmount); f("vigMidpoint", p.vigMidpoint); f("vigFeather", p.vigFeather);
  f("vigRoundness", p.vigRoundness);
  f("grainAmount", p.grainAmount); f("grainSize", p.grainSize); f("grainRoughness", p.grainRoughness);
  if (geom) {
    f("cropX", p.cropX); f("cropY", p.cropY); f("cropW", p.cropW); f("cropH", p.cropH);
    f("straighten", p.straighten);
    f("recover", p.recover);
    for (int i = 0; i < kMaxLayers; i++) {
      LocalLayer& l = p.layers[i];
      const char* names[9] = {"Feather", "Exposure", "Contrast", "Highlights", "Shadows", "Temp", "Tint", "Saturation", "Clarity"};
      float* vals[9] = {&l.feather, &l.exposure, &l.contrast, &l.highlights, &l.shadows, &l.temp, &l.tint, &l.saturation, &l.clarity};
      for (int k = 0; k < 9; k++) {
        snprintf(key, sizeof(key), "layer%d%s", i, names[k]);
        f(key, *vals[k]);
      }
    }
    for (int i = 0; i < kMaxImgLayers; i++) {
      ImageLayer& l = p.img[i];
      const char* names[9] = {"Cx", "Cy", "Scale", "Rotate", "Opacity", "Feather", "Exposure", "Temp", "Saturation"};
      float* vals[9] = {&l.cx, &l.cy, &l.scale, &l.rotate, &l.opacity, &l.feather, &l.exposure, &l.temp, &l.saturation};
      for (int k = 0; k < 9; k++) {
        snprintf(key, sizeof(key), "img%d%s", i, names[k]);
        f(key, *vals[k]);
      }
    }
  }
}

std::string serializeParams(const EditParams& pin, bool includeGeometry) {
  EditParams p = pin;
  std::string out;
  char line[128];
  visitScalars(p, includeGeometry, [&](const char* k, float& v) {
    if (k[0] == 'i' && k[1] == 'm' && k[2] == 'g' && !p.img[k[3] - '0'].enabled) return;  // unused photo-layer slot
    snprintf(line, sizeof(line), "%s=%.6g\n", k, v);
    out += line;
  });
  if (includeGeometry) {
    snprintf(line, sizeof(line), "rotate90=%d\nflipH=%d\nflipV=%d\nheals=%d\n", p.rotate90, p.flipH, p.flipV, p.heals);
    out += line;
    for (int i = 0; i < kMaxLayers; i++) {
      snprintf(line, sizeof(line), "layer%dEnabled=%d\nlayer%dRegion=%d\nlayer%dInvert=%d\n", i, p.layers[i].enabled, i,
               p.layers[i].region, i, p.layers[i].invert);
      out += line;
    }
    for (int i = 0; i < kMaxImgLayers; i++) {
      const ImageLayer& l = p.img[i];
      if (!l.enabled) continue;
      snprintf(line, sizeof(line), "img%dEnabled=1\nimg%dId=%d\nimg%dHidden=%d\nimg%dFlipH=%d\n", i, i, l.id, i, l.hidden, i,
               l.flipH);
      out += line;
      snprintf(line, sizeof(line), "img%dBlend=%d\nimg%dKeep=%d\nimg%dStrokes=%d\n", i, l.blend, i, l.keep, i, l.strokes);
      out += line;
      out += "img" + std::to_string(i) + "Path=" + l.path + "\n";
    }
  }
  for (int c = 0; c < CURVE_COUNT; c++) {
    snprintf(line, sizeof(line), "curve%d=", c);
    out += line;
    for (int i = 0; i < p.curve[c].n; i++) {
      snprintf(line, sizeof(line), "%s%.5g,%.5g", i ? ";" : "", p.curve[c].p[i].x, p.curve[c].p[i].y);
      out += line;
    }
    out += "\n";
  }
  return out;
}

static void parseCurve(const std::string& v, Curve& c) {
  Curve nc;
  std::memset(&nc, 0, sizeof(nc));
  std::stringstream ss(v);
  std::string pt;
  while (std::getline(ss, pt, ';') && nc.n < kMaxCurvePts) {
    size_t comma = pt.find(',');
    if (comma == std::string::npos) continue;
    float x = strtof(pt.c_str(), nullptr), y = strtof(pt.c_str() + comma + 1, nullptr);
    if (nc.n > 0 && x <= nc.p[nc.n - 1].x) continue;  // keep points sorted
    nc.p[nc.n++] = {x < 0 ? 0 : (x > 1 ? 1 : x), y < 0 ? 0 : (y > 1 ? 1 : y)};
  }
  if (nc.n >= 2) c = nc;
}

// ---------------------------------------------------------------- validation
// Sidecars and looks can come from anyone (a shared folder, a download). Every value is forced
// finite and into a range the renderer and the CPU code handle. Tone ranges are twice the
// sliders' because AI styles may exaggerate (strength up to 150%) and those files must load
// unchanged; geometry, sizes and counts get their exact limits.
static float fin(float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }
static int bit(int v) { return v ? 1 : 0; }

static void sanitizeCurve(Curve& c) {
  bool ok = c.n >= 2 && c.n <= kMaxCurvePts;
  for (int i = 0; ok && i < c.n; i++)
    ok = std::isfinite(c.p[i].x) && std::isfinite(c.p[i].y) && (i == 0 || c.p[i].x > c.p[i - 1].x);
  if (!ok) {
    std::memset((void*)&c, 0, sizeof(c));
    c.n = 2;
    c.p[1] = {1.f, 1.f};
    return;
  }
  for (int i = 0; i < c.n; i++) c.p[i] = {std::clamp(c.p[i].x, 0.f, 1.f), std::clamp(c.p[i].y, 0.f, 1.f)};
  for (int i = c.n; i < kMaxCurvePts; i++) c.p[i] = {0.f, 0.f};
}

void sanitizeParams(EditParams& p) {
  const EditParams d;
  const float T = 200.f;  // tone sliders are +-100 in the UI
  for (float* v : {&p.temp, &p.tint, &p.contrast, &p.highlights, &p.shadows, &p.whites, &p.blacks, &p.texture,
                   &p.clarity, &p.dehaze, &p.vibrance, &p.saturation, &p.vigAmount, &p.vigRoundness})
    *v = fin(*v, -T, T, 0.f);
  p.exposure = fin(p.exposure, -10.f, 10.f, 0.f);
  for (int i = 0; i < kHslBands; i++) {
    p.hslHue[i] = fin(p.hslHue[i], -T, T, 0.f);
    p.hslSat[i] = fin(p.hslSat[i], -T, T, 0.f);
    p.hslLum[i] = fin(p.hslLum[i], -T, T, 0.f);
  }
  for (GradeWheel& g : p.grade) {
    g.hue = std::isfinite(g.hue) ? std::fmod(std::fmod(g.hue, 360.f) + 360.f, 360.f) : 0.f;
    g.sat = fin(g.sat, 0.f, T, 0.f);
    g.lum = fin(g.lum, -T, T, 0.f);
  }
  p.gradeBlending = fin(p.gradeBlending, 0.f, 100.f, d.gradeBlending);
  p.gradeBalance = fin(p.gradeBalance, -100.f, 100.f, 0.f);
  for (Curve& c : p.curve) sanitizeCurve(c);
  p.sharpAmount = fin(p.sharpAmount, 0.f, 300.f, 0.f);
  p.sharpRadius = fin(p.sharpRadius, 0.5f, 3.f, d.sharpRadius);
  p.sharpMasking = fin(p.sharpMasking, 0.f, 100.f, 0.f);
  p.nrLum = fin(p.nrLum, 0.f, T, 0.f);
  p.nrColor = fin(p.nrColor, 0.f, T, 0.f);
  p.vigMidpoint = fin(p.vigMidpoint, 0.f, 100.f, d.vigMidpoint);
  p.vigFeather = fin(p.vigFeather, 0.f, 100.f, d.vigFeather);
  p.grainAmount = fin(p.grainAmount, 0.f, T, 0.f);
  p.grainSize = fin(p.grainSize, 0.f, 100.f, d.grainSize);
  p.grainRoughness = fin(p.grainRoughness, 0.f, 100.f, d.grainRoughness);
  // Geometry: the crop decides the export size, so it must stay inside the frame.
  p.rotate90 &= 3;
  p.flipH = bit(p.flipH);
  p.flipV = bit(p.flipV);
  p.straighten = fin(p.straighten, -45.f, 45.f, 0.f);
  bool cropOk = std::isfinite(p.cropX) && std::isfinite(p.cropY) && std::isfinite(p.cropW) && std::isfinite(p.cropH);
  if (!cropOk) p.cropX = p.cropY = 0.f, p.cropW = p.cropH = 1.f;
  p.cropW = std::clamp(p.cropW, 0.01f, 1.f);
  p.cropH = std::clamp(p.cropH, 0.01f, 1.f);
  p.cropX = std::clamp(p.cropX, 0.f, 1.f - p.cropW);
  p.cropY = std::clamp(p.cropY, 0.f, 1.f - p.cropH);
  p.recover = fin(p.recover, 0.f, 100.f, 0.f);
  // AI local layers (the region index is checked again where masks are built)
  for (LocalLayer& l : p.layers) {
    l.enabled = bit(l.enabled);
    l.invert = bit(l.invert);
    if (l.region < 0 || l.region >= kMaxRegionId) l.enabled = 0, l.region = 0;
    l.feather = fin(l.feather, 0.f, 100.f, 10.f);
    l.exposure = fin(l.exposure, -6.f, 6.f, 0.f);
    for (float* v : {&l.contrast, &l.highlights, &l.shadows, &l.temp, &l.tint, &l.saturation, &l.clarity})
      *v = fin(*v, -T, T, 0.f);
  }
  // Photo layers: one asset per id, so ids must be positive and unique.
  int maxId = 0;
  for (const ImageLayer& l : p.img)
    if (l.enabled && l.id > 0 && l.id < kMaxLayerId) maxId = std::max(maxId, l.id);
  for (int i = 0; i < kMaxImgLayers; i++) {
    ImageLayer& l = p.img[i];
    l.path[kLayerPathMax - 1] = 0;
    l.enabled = bit(l.enabled);
    bool dup = false;
    for (int j = 0; j < i; j++) dup |= p.img[j].enabled && p.img[j].id == l.id;
    if (l.enabled && (l.id <= 0 || l.id >= kMaxLayerId || dup)) l.id = ++maxId, l.strokes = 0;
    if (!l.path[0] || l.id >= kMaxLayerId) l.enabled = 0;
    l.hidden = bit(l.hidden);
    l.flipH = bit(l.flipH);
    l.cx = fin(l.cx, -10.f, 10.f, 0.5f);
    l.cy = fin(l.cy, -10.f, 10.f, 0.5f);
    l.scale = fin(l.scale, 0.001f, 50.f, 0.5f);
    l.rotate = std::isfinite(l.rotate) ? std::remainder(l.rotate, 360.f) : 0.f;
    if (l.blend < 0 || l.blend >= BLEND_COUNT) l.blend = BLEND_NORMAL;
    l.opacity = fin(l.opacity, 0.f, 100.f, 100.f);
    l.feather = fin(l.feather, 0.f, 100.f, 5.f);
    l.strokes = std::clamp(l.strokes, 0, kMaxLayerStrokes);
    l.exposure = fin(l.exposure, -5.f, 5.f, 0.f);
    l.temp = fin(l.temp, -100.f, 100.f, 0.f);
    l.saturation = fin(l.saturation, -100.f, 100.f, 0.f);
  }
  p.heals = std::clamp(p.heals, 0, kMaxHeals);
}

void deserializeParams(const std::string& text, EditParams& p, bool includeGeometry) {
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k.size() == 6 && k.compare(0, 5, "curve") == 0 && k[5] >= '0' && k[5] < '0' + CURVE_COUNT) {
      parseCurve(v, p.curve[k[5] - '0']);
      continue;
    }
    if (includeGeometry) {
      if (k == "rotate90") { p.rotate90 = atoi(v.c_str()) & 3; continue; }
      if (k == "flipH") { p.flipH = atoi(v.c_str()) ? 1 : 0; continue; }
      if (k == "flipV") { p.flipV = atoi(v.c_str()) ? 1 : 0; continue; }
      if (k == "heals") { p.heals = std::max(0, atoi(v.c_str())); continue; }
      if (k.size() > 6 && k.compare(0, 5, "layer") == 0 && k[5] >= '0' && k[5] < '0' + kMaxLayers) {
        LocalLayer& l = p.layers[k[5] - '0'];
        std::string field = k.substr(6);
        if (field == "Enabled") { l.enabled = atoi(v.c_str()) ? 1 : 0; continue; }
        if (field == "Region") { l.region = atoi(v.c_str()); continue; }
        if (field == "Invert") { l.invert = atoi(v.c_str()) ? 1 : 0; continue; }
      }
      if (k.size() > 4 && k.compare(0, 3, "img") == 0 && k[3] >= '0' && k[3] < '0' + kMaxImgLayers) {
        ImageLayer& l = p.img[k[3] - '0'];
        std::string field = k.substr(4);
        int iv = atoi(v.c_str());
        if (field == "Enabled") { l.enabled = iv ? 1 : 0; continue; }
        if (field == "Id") { l.id = iv; continue; }
        if (field == "Hidden") { l.hidden = iv ? 1 : 0; continue; }
        if (field == "FlipH") { l.flipH = iv ? 1 : 0; continue; }
        if (field == "Blend") { l.blend = iv >= 0 && iv < BLEND_COUNT ? iv : 0; continue; }
        if (field == "Keep") { l.keep = iv; continue; }
        if (field == "Strokes") { l.strokes = iv > 0 ? iv : 0; continue; }
        if (field == "Path") {  // too long for the field: dropped, never cut mid-character
          std::memset(l.path, 0, sizeof(l.path));
          if (v.size() < sizeof(l.path) && v.find('\0') == std::string::npos) std::memcpy(l.path, v.data(), v.size());
          continue;
        }
      }
    }
    visitScalars(p, includeGeometry, [&](const char* key, float& f) {
      if (k == key) f = strtof(v.c_str(), nullptr);
    });
  }
  sanitizeParams(p);
}

std::vector<Preset> builtinPresets() {
  return {
      {"Vivid", "สดใส",
       "contrast=20\nhighlights=-15\nshadows=15\nclarity=10\nvibrance=35\nsaturation=8\n"},
      {"Matte Film", "ฟิล์มแมตต์",
       "contrast=-10\nsaturation=-15\ncurve0=0,0.08;0.25,0.25;0.75,0.79;1,0.95\n"
       "grade0Hue=205\ngrade0Sat=18\ngrade2Hue=40\ngrade2Sat=12\ngrainAmount=18\n"},
      {"B&W Contrast", "ขาวดำคอนทราสต์",
       "saturation=-100\ncontrast=35\nclarity=15\nwhites=10\nblacks=-15\nvigAmount=-15\n"},
      {"Warm Sunset", "อบอุ่นยามเย็น",
       "temp=25\ntint=8\nvibrance=20\nhighlights=-20\ngrade2Hue=35\ngrade2Sat=22\nvigAmount=-20\n"},
      {"Cool Moody", "เย็นหม่น",
       "temp=-18\nexposure=-0.2\ncontrast=15\nhighlights=-30\nsaturation=-12\ngrade0Hue=210\ngrade0Sat=25\n"
       "vigAmount=-25\n"},
      {"Soft Portrait", "พอร์ตเทรตนุ่ม",
       "exposure=0.15\ncontrast=-8\nhighlights=-10\nshadows=10\ntexture=-15\nclarity=-10\nvibrance=10\n"},
  };
}

void applyPreset(const Preset& pr, EditParams& p) {
  EditParams np;
  copyGeometry(np, p);
  copyLayers(np, p);
  deserializeParams(pr.data, np, false);
  p = np;
}

std::string sidecarPath(const std::string& photo) { return photo + kSidecarExt; }

// Real sidecars are a few KB (brush strokes add up to a few MB); anything far bigger is not ours.
static bool readSmallText(const std::string& path, std::string& text) {
  unsigned long long size = 0;
  long long mtime = 0;
  if (!fileInfo(path, size, mtime) || size > kMaxSidecarBytes) return false;
  return readTextFile(path, text);
}

bool readSidecar(const std::string& photo, std::string& text) {
  return readSmallText(photo + kSidecarExt, text) || readSmallText(photo + kLegacySidecarExt, text);
}

void writeSidecar(const std::string& photo, const std::string& text) {
  std::string sc = photo + kSidecarExt, old = photo + kLegacySidecarExt;
  if (text.empty()) {
    if (fileExists(sc)) deleteFileU(sc);
  } else {
    writeTextFile(sc, text);
  }
  if (fileExists(old)) deleteFileU(old);
}
