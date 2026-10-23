#include "params.h"

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
    snprintf(line, sizeof(line), "rotate90=%d\nflipH=%d\nflipV=%d\n", p.rotate90, p.flipH, p.flipV);
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
        if (field == "Path") {
          std::memset(l.path, 0, sizeof(l.path));
          std::memcpy(l.path, v.data(), std::min(v.size(), sizeof(l.path) - 1));
          continue;
        }
      }
    }
    visitScalars(p, includeGeometry, [&](const char* key, float& f) {
      if (k == key) f = strtof(v.c_str(), nullptr);
    });
  }
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

bool readSidecar(const std::string& photo, std::string& text) {
  return readTextFile(photo + kSidecarExt, text) || readTextFile(photo + kLegacySidecarExt, text);
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
