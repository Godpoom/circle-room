#include "font_maker.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <unordered_map>

#include "image_io.h"
#include "layer_content.h"
#include "platform_win.h"
#include "updater.h"  // sha256Hex

// ---------------------------------------------------------------- character set
const std::vector<uint32_t>& fontMakerChars() {
  static std::vector<uint32_t> v;
  if (v.empty()) {
    for (uint32_t c = 0x0E01; c <= 0x0E2E; c++) v.push_back(c);  // consonants
    for (uint32_t c = 0x0E2F; c <= 0x0E3A; c++) v.push_back(c);  // vowels
    v.push_back(0x0E3F);                                          // baht
    for (uint32_t c = 0x0E40; c <= 0x0E4E; c++) v.push_back(c);  // leading vowels, tone marks
    for (uint32_t c = 0x0E50; c <= 0x0E59; c++) v.push_back(c);  // Thai digits
    for (uint32_t c = 'A'; c <= 'Z'; c++) v.push_back(c);
    for (uint32_t c = 'a'; c <= 'z'; c++) v.push_back(c);
    for (uint32_t c = '0'; c <= '9'; c++) v.push_back(c);
    for (const char* p = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"; *p; p++) v.push_back((uint32_t)(unsigned char)*p);
  }
  return v;
}

bool isThaiMark(uint32_t cp) { return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E); }

// ---------------------------------------------------------------- projects
namespace {

std::string projectsRoot() { return dirOf(userFontDir()) + "\\fontprojects"; }

std::string safeName(const std::string& name) {
  std::string s;
  for (char c : name)
    if (isalnum((unsigned char)c)) s += c;
  if (s.size() > 24) s.resize(24);
  return (s.empty() ? std::string("font") : s) + "_" + sha256Hex(name.data(), name.size()).substr(0, 8);
}

std::string glyphFile(const FontProject& p, uint32_t cp) {
  char b[16];
  snprintf(b, sizeof(b), "%04X.png", cp);
  return p.dir + "\\" + b;
}

bool mkdirs(const std::string& d) {
  if (d.size() < 3 || isDirectory(d)) return true;
  if (!mkdirs(dirOf(d))) return false;
  return ensureDir(d) || isDirectory(d);
}

}  // namespace

std::vector<std::string> listFontProjects() {
  std::vector<std::string> out;
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileW(utf8ToWide(projectsRoot() + "\\*").c_str(), &fd);
  if (f == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
    std::string name;
    if (readTextFile(projectsRoot() + "\\" + wideToUtf8(fd.cFileName) + "\\name.txt", name) && !name.empty())
      out.push_back(name.substr(0, 64));
  } while (FindNextFileW(f, &fd));
  FindClose(f);
  std::sort(out.begin(), out.end());
  return out;
}

bool openFontProject(const std::string& name0, FontProject& p, std::string& err) {
  std::string name = name0.substr(0, 64);
  while (!name.empty() && (name.back() == ' ')) name.pop_back();
  if (name.empty()) {
    err = "the font needs a name";
    return false;
  }
  p = FontProject();
  p.name = name;
  p.dir = projectsRoot() + "\\" + safeName(name);
  if (!mkdirs(p.dir) || !writeTextFile(p.dir + "\\name.txt", name)) {
    err = "cannot create " + p.dir;
    return false;
  }
  const std::vector<uint32_t>& cs = fontMakerChars();
  p.ink.assign(cs.size(), {});
  for (size_t i = 0; i < cs.size(); i++) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!fileExists(glyphFile(p, cs[i])) || !loadRgbaFile(glyphFile(p, cs[i]), rgba, w, h) || w != kGlyphCell || h != kGlyphCell)
      continue;
    p.ink[i].resize((size_t)w * h);
    for (size_t k = 0; k < p.ink[i].size(); k++) p.ink[i][k] = rgba[k * 4 + 3];
  }
  return true;
}

bool saveGlyph(const FontProject& p, size_t index, std::string& err) {
  if (index >= p.ink.size()) return false;
  std::string file = glyphFile(p, fontMakerChars()[index]);
  bool any = false;
  for (uint8_t v : p.ink[index]) any |= v != 0;
  if (!any) {
    deleteFileU(file);
    return true;
  }
  std::vector<uint8_t> rgba((size_t)kGlyphCell * kGlyphCell * 4, 0);
  for (size_t k = 0; k < p.ink[index].size(); k++) rgba[k * 4 + 3] = p.ink[index][k];
  return writePngRgba(file, rgba.data(), kGlyphCell, kGlyphCell, err);
}

int drawnGlyphs(const FontProject& p) {
  int n = 0;
  for (const auto& g : p.ink) {
    bool any = false;
    for (uint8_t v : g) any |= v > 127;
    n += any;
  }
  return n;
}

// ---------------------------------------------------------------- importing images
namespace {

// Ink coverage from an image: its transparency if it has any, else how dark each pixel is.
std::vector<uint8_t> inkOf(const std::vector<uint8_t>& rgba, int w, int h) {
  bool alpha = false;
  for (size_t i = 0; i < (size_t)w * h && !alpha; i++) alpha = rgba[i * 4 + 3] < 250;
  std::vector<uint8_t> ink((size_t)w * h);
  for (size_t i = 0; i < ink.size(); i++) {
    const uint8_t* q = &rgba[i * 4];
    float L = 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2];
    float v = alpha ? q[3] * (1.f - L / 400.f) : std::clamp((190.f - L) * 255.f / 130.f, 0.f, 255.f);
    ink[i] = (uint8_t)std::clamp(v, 0.f, 255.f);
  }
  return ink;
}

// Bilinear sample of an 8-bit image at (x, y) in pixels.
float sample(const std::vector<uint8_t>& im, int w, int h, float x, float y) {
  x = std::clamp(x - 0.5f, 0.f, (float)w - 1);
  y = std::clamp(y - 0.5f, 0.f, (float)h - 1);
  int x0 = (int)x, y0 = (int)y, x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
  float fx = x - x0, fy = y - y0;
  float a = im[(size_t)y0 * w + x0] * (1 - fx) + im[(size_t)y0 * w + x1] * fx;
  float b = im[(size_t)y1 * w + x0] * (1 - fx) + im[(size_t)y1 * w + x1] * fx;
  return a * (1 - fy) + b * fy;
}

}  // namespace

bool importGlyphImage(const std::string& file, std::vector<uint8_t>& out, std::string& err) {
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!loadRgbaFile(file, rgba, w, h) || w <= 0 || h <= 0) {
    err = "cannot read " + fileName(file);
    return false;
  }
  std::vector<uint8_t> ink = inkOf(rgba, w, h);
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (ink[(size_t)y * w + x] > 64) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
      }
  if (x1 < 0) {
    err = "no ink found in the image";
    return false;
  }
  // Fit the ink between the cap height and the baseline, centred.
  float bw = (float)(x1 - x0 + 1), bh = (float)(y1 - y0 + 1);
  float k = std::min((kGlyphBaseline - kGlyphCapHeight) / bh, 220.f / bw);
  float ox = (kGlyphCell - bw * k) * 0.5f, oy = kGlyphBaseline - bh * k;
  out.assign((size_t)kGlyphCell * kGlyphCell, 0);
  for (int y = 0; y < kGlyphCell; y++)
    for (int x = 0; x < kGlyphCell; x++) {
      float sx = x0 + (x + 0.5f - ox) / k, sy = y0 + (y + 0.5f - oy) / k;
      if (sx < x0 || sy < y0 || sx > x1 + 1 || sy > y1 + 1) continue;
      out[(size_t)y * kGlyphCell + x] = (uint8_t)std::lround(sample(ink, w, h, sx, sy));
    }
  return true;
}

namespace {
const int kSheetCols = 12, kSheetCell = 200, kSheetLabel = 34;
}

bool writeTemplateSheet(const std::string& path, std::string& err) {
  const std::vector<uint32_t>& cs = fontMakerChars();
  int rows = (int)((cs.size() + kSheetCols - 1) / kSheetCols);
  int W = kSheetCols * kSheetCell, H = rows * (kSheetCell + kSheetLabel);
  std::vector<uint8_t> rgb((size_t)W * H * 3, 255);
  auto put = [&](int x, int y, uint8_t v) {
    if (x >= 0 && y >= 0 && x < W && y < H) std::memset(&rgb[((size_t)y * W + x) * 3], v, 3);
  };
  float k = (float)kSheetCell / kGlyphCell;
  for (size_t i = 0; i < cs.size(); i++) {
    int cx = (int)(i % kSheetCols) * kSheetCell, cy = (int)(i / kSheetCols) * (kSheetCell + kSheetLabel);
    // the character's name above its cell
    TextSpec t;
    t.font = "Tahoma";
    t.color = 0x787878FF;
    wchar_t wc[3] = {(wchar_t)(isThaiMark(cs[i]) ? 0x25CC : cs[i]), (wchar_t)(isThaiMark(cs[i]) ? cs[i] : 0), 0};
    t.text = wideToUtf8(wc);
    std::vector<uint8_t> trgb, ta;
    int tw = 0, th = 0;
    if (renderText(t, trgb, ta, tw, th, err)) {
      float s = std::min((kSheetLabel - 6.f) / th, 60.f / tw);
      int dw = std::max(1, (int)(tw * s)), dh = std::max(1, (int)(th * s));
      for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
          int a = ta[(size_t)std::min(th - 1, (int)(y / s)) * tw + std::min(tw - 1, (int)(x / s))];
          if (a > 40) put(cx + 8 + x, cy + 3 + y, (uint8_t)(255 - a * 135 / 255));
        }
    }
    int top = cy + kSheetLabel;
    for (int x = 0; x < kSheetCell; x++) {
      put(cx + x, top, 150);
      put(cx + x, top + kSheetCell - 1, 150);
      if (x % 6 < 3) {  // dashed guides, light enough that they vanish on import
        put(cx + x, top + (int)(kGlyphBaseline * k), 205);
        put(cx + x, top + (int)(kGlyphXHeight * k), 228);
        put(cx + x, top + (int)(kGlyphCapHeight * k), 228);
      }
    }
    for (int y = 0; y < kSheetCell; y++) {
      put(cx, top + y, 150);
      put(cx + kSheetCell - 1, top + y, 150);
    }
  }
  return writeImage(path, rgb.data(), W, H, true, 92, err);
}

int importTemplateSheet(const std::string& file, FontProject& p, std::string& err) {
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!loadRgbaFile(file, rgba, w, h) || w <= 0 || h <= 0) {
    err = "cannot read " + fileName(file);
    return -1;
  }
  const std::vector<uint32_t>& cs = fontMakerChars();
  int rows = (int)((cs.size() + kSheetCols - 1) / kSheetCols);
  int W = kSheetCols * kSheetCell, H = rows * (kSheetCell + kSheetLabel);
  std::vector<uint8_t> ink = inkOf(rgba, w, h);
  float sx = (float)w / W, sy = (float)h / H;  // a scan of any size, cropped to the sheet's border
  float k = (float)kSheetCell / kGlyphCell;
  int filled = 0;
  for (size_t i = 0; i < cs.size() && i < p.ink.size(); i++) {
    int cx = (int)(i % kSheetCols) * kSheetCell, cy = (int)(i / kSheetCols) * (kSheetCell + kSheetLabel) + kSheetLabel;
    std::vector<uint8_t> g((size_t)kGlyphCell * kGlyphCell, 0);
    int inked = 0;
    for (int y = 3; y < kGlyphCell - 3; y++)  // stay off the printed border
      for (int x = 3; x < kGlyphCell - 3; x++) {
        float v = sample(ink, w, h, (cx + (x + 0.5f) * k) * sx, (cy + (y + 0.5f) * k) * sy);
        uint8_t b = (uint8_t)std::lround(v);
        g[(size_t)y * kGlyphCell + x] = b;
        inked += b > 128;
      }
    if (inked < 30) continue;  // left empty
    p.ink[i].swap(g);
    saveGlyph(p, i, err);
    filled++;
  }
  return filled;
}

// ---------------------------------------------------------------- tracing (marching squares)
std::vector<std::vector<std::pair<float, float>>> traceBitmap(const std::vector<uint8_t>& ink, int w, int h) {
  const float T = 127.5f;
  auto V = [&](int x, int y) -> float { return x < 0 || y < 0 || x >= w || y >= h ? 0.f : ink[(size_t)y * w + x]; };
  // Edge ids: horizontal edge from corner (i, j) to (i+1, j) and vertical edge (i, j) to (i, j+1),
  // corners at pixel centres, i and j from -1 to w / h.
  auto key = [&](int i, int j, int vert) -> uint64_t { return ((uint64_t)(j + 1) * (uint64_t)(w + 2) + (uint64_t)(i + 1)) * 2 + vert; };
  std::unordered_map<uint64_t, std::pair<float, float>> pos;
  std::unordered_map<uint64_t, std::vector<uint64_t>> adj;
  auto edgePoint = [&](int i, int j, int vert) {
    uint64_t k = key(i, j, vert);
    if (!pos.count(k)) {
      float a = V(i, j), b = vert ? V(i, j + 1) : V(i + 1, j);
      float t = std::fabs(b - a) < 1e-6f ? 0.5f : std::clamp((T - a) / (b - a), 0.f, 1.f);
      pos[k] = vert ? std::make_pair(i + 0.5f, j + 0.5f + t) : std::make_pair(i + 0.5f + t, j + 0.5f);
    }
    return k;
  };
  auto link = [&](uint64_t a, uint64_t b) {
    adj[a].push_back(b);
    adj[b].push_back(a);
  };
  for (int j = -1; j < h; j++)
    for (int i = -1; i < w; i++) {
      float tl = V(i, j), tr = V(i + 1, j), br = V(i + 1, j + 1), bl = V(i, j + 1);
      int c = (tl >= T) | (tr >= T) << 1 | (br >= T) << 2 | (bl >= T) << 3;
      if (c == 0 || c == 15) continue;
      auto e0 = [&] { return edgePoint(i, j, 0); };      // top
      auto e1 = [&] { return edgePoint(i + 1, j, 1); };  // right
      auto e2 = [&] { return edgePoint(i, j + 1, 0); };  // bottom
      auto e3 = [&] { return edgePoint(i, j, 1); };      // left
      bool centre = (tl + tr + br + bl) * 0.25f >= T;
      switch (c) {
        case 1: case 14: link(e3(), e0()); break;
        case 2: case 13: link(e0(), e1()); break;
        case 3: case 12: link(e3(), e1()); break;
        case 4: case 11: link(e1(), e2()); break;
        case 6: case 9: link(e0(), e2()); break;
        case 7: case 8: link(e3(), e2()); break;
        case 5:
          if (centre) { link(e0(), e1()); link(e2(), e3()); }
          else { link(e3(), e0()); link(e1(), e2()); }
          break;
        case 10:
          if (centre) { link(e3(), e0()); link(e1(), e2()); }
          else { link(e0(), e1()); link(e2(), e3()); }
          break;
      }
    }
  std::vector<std::vector<std::pair<float, float>>> loops;
  std::unordered_map<uint64_t, bool> used;
  for (auto& kv : adj) {
    if (used[kv.first]) continue;
    std::vector<std::pair<float, float>> loop;
    uint64_t prev = UINT64_MAX, cur = kv.first;
    for (size_t guard = 0; guard < adj.size() + 2; guard++) {
      used[cur] = true;
      loop.push_back(pos[cur]);
      const std::vector<uint64_t>& n = adj[cur];
      uint64_t next = UINT64_MAX;
      for (uint64_t c2 : n)
        if (c2 != prev && !used[c2]) {
          next = c2;
          break;
        }
      if (next == UINT64_MAX) break;
      prev = cur;
      cur = next;
    }
    if (loop.size() >= 3) loops.push_back(loop);
  }
  // Simplify each closed loop (Douglas-Peucker, 0.35 px) and drop specks.
  auto area = [](const std::vector<std::pair<float, float>>& L) {
    double s = 0;
    for (size_t i = 0; i < L.size(); i++) {
      const auto& p = L[i];
      const auto& q = L[(i + 1) % L.size()];
      s += (double)p.first * q.second - (double)q.first * p.second;
    }
    return s * 0.5;
  };
  std::vector<std::vector<std::pair<float, float>>> out;
  for (auto& L : loops) {
    if (std::fabs(area(L)) < 3.0) continue;
    std::vector<char> keep(L.size(), 0);
    size_t farIdx = 0;
    float best = -1;
    for (size_t i = 1; i < L.size(); i++) {
      float d = std::hypot(L[i].first - L[0].first, L[i].second - L[0].second);
      if (d > best) best = d, farIdx = i;
    }
    keep[0] = keep[farIdx] = 1;
    std::vector<std::pair<size_t, size_t>> stack = {{0, farIdx}, {farIdx, L.size()}};
    while (!stack.empty()) {
      auto [a, b] = stack.back();
      stack.pop_back();
      const auto& A = L[a];
      const auto& B = L[b % L.size()];
      float dx = B.first - A.first, dy = B.second - A.second, len = std::max(1e-6f, std::hypot(dx, dy));
      size_t idx = 0;
      float dmax = 0;
      for (size_t i = a + 1; i < b; i++) {
        float d = std::fabs((L[i].first - A.first) * dy - (L[i].second - A.second) * dx) / len;
        if (d > dmax) dmax = d, idx = i;
      }
      if (dmax > 0.35f) {
        keep[idx] = 1;
        stack.push_back({a, idx});
        stack.push_back({idx, b});
      }
    }
    std::vector<std::pair<float, float>> s;
    for (size_t i = 0; i < L.size(); i++)
      if (keep[i]) s.push_back(L[i]);
    if (s.size() >= 3) out.push_back(s);
  }
  return out;
}

// ---------------------------------------------------------------- TrueType writer
namespace {
void be16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
  be16(v, x >> 16);
  be16(v, x & 0xFFFF);
}
uint32_t checksum(const std::vector<uint8_t>& d) {
  uint32_t s = 0;
  for (size_t i = 0; i < d.size(); i += 4) {
    uint32_t w = 0;
    for (int k = 0; k < 4; k++) w = w << 8 | (i + k < d.size() ? d[i + k] : 0);
    s += w;
  }
  return s;
}
void utf16be(std::vector<uint8_t>& v, const std::string& s) {
  for (wchar_t c : utf8ToWide(s)) be16(v, (uint16_t)c);
}
}  // namespace

std::vector<uint8_t> writeTrueType(const std::string& family, const std::vector<FmGlyph>& G) {
  const int kAsc = 850, kDesc = -250, kGap = 100;
  // glyf + loca
  std::vector<uint8_t> glyf, loca, hmtx;
  int xMin = 0, yMin = 0, xMax = 0, yMax = 0, maxPts = 0, maxCont = 0, advMax = 0, minLsb = 32767, minRsb = 32767, xMaxExt = 0;
  bool anyBox = false;
  long long advSum = 0;
  int advN = 0;
  for (const FmGlyph& g : G) {
    be32(loca, (uint32_t)glyf.size());
    int gx0 = 32767, gy0 = 32767, gx1 = -32768, gy1 = -32768, npts = 0;
    for (const auto& c : g.contours)
      for (const auto& p : c) {
        gx0 = std::min(gx0, p.first);
        gy0 = std::min(gy0, p.second);
        gx1 = std::max(gx1, p.first);
        gy1 = std::max(gy1, p.second);
        npts++;
      }
    advMax = std::max(advMax, g.advance);
    if (g.advance > 0) advSum += g.advance, advN++;
    if (npts) {
      be16(glyf, (uint16_t)(int16_t)g.contours.size());
      be16(glyf, (uint16_t)(int16_t)gx0);
      be16(glyf, (uint16_t)(int16_t)gy0);
      be16(glyf, (uint16_t)(int16_t)gx1);
      be16(glyf, (uint16_t)(int16_t)gy1);
      int end = -1;
      for (const auto& c : g.contours) {
        end += (int)c.size();
        be16(glyf, (uint16_t)end);
      }
      be16(glyf, 0);  // no instructions
      for (int k = 0; k < npts; k++) glyf.push_back(0x01);  // on-curve, int16 deltas
      int px = 0, py = 0;
      for (const auto& c : g.contours)
        for (const auto& p : c) {
          be16(glyf, (uint16_t)(int16_t)(p.first - px));
          px = p.first;
        }
      for (const auto& c : g.contours)
        for (const auto& p : c) {
          be16(glyf, (uint16_t)(int16_t)(p.second - py));
          py = p.second;
        }
      while (glyf.size() % 4) glyf.push_back(0);
      if (!anyBox) xMin = gx0, yMin = gy0, xMax = gx1, yMax = gy1, anyBox = true;
      xMin = std::min(xMin, gx0);
      yMin = std::min(yMin, gy0);
      xMax = std::max(xMax, gx1);
      yMax = std::max(yMax, gy1);
      maxPts = std::max(maxPts, npts);
      maxCont = std::max(maxCont, (int)g.contours.size());
      minLsb = std::min(minLsb, gx0);
      minRsb = std::min(minRsb, g.advance - gx1);
      xMaxExt = std::max(xMaxExt, gx1);
    }
    be16(hmtx, (uint16_t)g.advance);
    be16(hmtx, (uint16_t)(int16_t)(npts ? gx0 : 0));
  }
  be32(loca, (uint32_t)glyf.size());
  if (minLsb == 32767) minLsb = minRsb = 0;
  uint16_t nG = (uint16_t)G.size();

  // cmap (format 4: one segment per character)
  std::vector<std::pair<uint32_t, uint16_t>> map;
  for (size_t i = 0; i < G.size(); i++)
    if (G[i].cp > 0 && G[i].cp < 0xFFFF) map.push_back({G[i].cp, (uint16_t)i});
  std::sort(map.begin(), map.end());
  uint16_t segs = (uint16_t)(map.size() + 1);
  uint16_t sr = 1, es = 0;
  while (sr * 2 <= segs) sr *= 2, es++;
  sr *= 2;
  std::vector<uint8_t> sub;
  be16(sub, 4);
  be16(sub, (uint16_t)(16 + 8 * segs));
  be16(sub, 0);
  be16(sub, segs * 2);
  be16(sub, sr);
  be16(sub, es);
  be16(sub, (uint16_t)(segs * 2 - sr));
  for (auto& m : map) be16(sub, m.first);
  be16(sub, 0xFFFF);
  be16(sub, 0);
  for (auto& m : map) be16(sub, m.first);
  be16(sub, 0xFFFF);
  for (auto& m : map) be16(sub, (uint16_t)((m.second - m.first) & 0xFFFF));
  be16(sub, 1);
  for (uint16_t k = 0; k < segs; k++) be16(sub, 0);
  std::vector<uint8_t> cmap;
  be16(cmap, 0);
  be16(cmap, 1);
  be16(cmap, 3);
  be16(cmap, 1);
  be32(cmap, 12);
  cmap.insert(cmap.end(), sub.begin(), sub.end());

  // head (checkSumAdjustment patched last)
  std::vector<uint8_t> head;
  be16(head, 1);
  be16(head, 0);
  be32(head, 0x00010000);
  be32(head, 0);
  be32(head, 0x5F0F3CF5);
  be16(head, 0x000B);
  be16(head, 1000);
  uint64_t now = (uint64_t)time(nullptr) + 2082844800ull;
  be32(head, (uint32_t)(now >> 32));
  be32(head, (uint32_t)now);
  be32(head, (uint32_t)(now >> 32));
  be32(head, (uint32_t)now);
  be16(head, (uint16_t)(int16_t)xMin);
  be16(head, (uint16_t)(int16_t)yMin);
  be16(head, (uint16_t)(int16_t)xMax);
  be16(head, (uint16_t)(int16_t)yMax);
  be16(head, 0);
  be16(head, 8);
  be16(head, 2);
  be16(head, 1);  // long loca
  be16(head, 0);

  std::vector<uint8_t> hhea;
  be16(hhea, 1);
  be16(hhea, 0);
  be16(hhea, (uint16_t)kAsc);
  be16(hhea, (uint16_t)(int16_t)kDesc);
  be16(hhea, kGap);
  be16(hhea, (uint16_t)advMax);
  be16(hhea, (uint16_t)(int16_t)minLsb);
  be16(hhea, (uint16_t)(int16_t)minRsb);
  be16(hhea, (uint16_t)(int16_t)xMaxExt);
  be16(hhea, 1);
  be16(hhea, 0);
  be16(hhea, 0);
  for (int k = 0; k < 4; k++) be16(hhea, 0);
  be16(hhea, 0);
  be16(hhea, nG);

  std::vector<uint8_t> maxp;
  be32(maxp, 0x00010000);
  be16(maxp, nG);
  be16(maxp, (uint16_t)maxPts);
  be16(maxp, (uint16_t)maxCont);
  be16(maxp, 0);
  be16(maxp, 0);
  be16(maxp, 2);
  for (int k = 0; k < 8; k++) be16(maxp, 0);  // twilight, storage, fdefs, idefs, stack, instr size, components, depth

  bool thai = false;
  uint32_t first = 0xFFFF, last = 0;
  for (auto& m : map) {
    thai |= m.first >= 0x0E00 && m.first <= 0x0E7F;
    first = std::min(first, m.first);
    last = std::max(last, m.first);
  }
  std::vector<uint8_t> os2;
  be16(os2, 4);
  be16(os2, (uint16_t)(advN ? advSum / advN : 500));
  be16(os2, 400);
  be16(os2, 5);
  be16(os2, 0);  // installable
  for (int v : {650, 600, 0, 75, 650, 600, 0, 350, 50, 300}) be16(os2, (uint16_t)(int16_t)v);
  be16(os2, 0);
  for (int k = 0; k < 10; k++) os2.push_back(0);
  be32(os2, 1u | (thai ? 1u << 24 : 0u));
  be32(os2, 0);
  be32(os2, 0);
  be32(os2, 0);
  for (char c : std::string("CRRM")) os2.push_back((uint8_t)c);
  be16(os2, 0x40);
  be16(os2, (uint16_t)std::min<uint32_t>(first, 0xFFFF));
  be16(os2, (uint16_t)std::min<uint32_t>(last, 0xFFFF));
  be16(os2, (uint16_t)kAsc);
  be16(os2, (uint16_t)(int16_t)kDesc);
  be16(os2, kGap);
  be16(os2, (uint16_t)std::max(1000, yMax));
  be16(os2, (uint16_t)std::max(300, -yMin));
  be32(os2, 1u | (thai ? 1u << 16 : 0u));
  be32(os2, 0);
  be16(os2, 500);
  be16(os2, 700);
  be16(os2, 0);
  be16(os2, 32);
  be16(os2, 0);

  // name
  std::string ps;
  for (char c : family)
    if (isalnum((unsigned char)c)) ps += c;
  if (ps.empty()) ps = "CircleRoomFont";
  ps += "-Regular";
  std::vector<std::pair<int, std::string>> names = {
      {0, "Made with Circle Room"}, {1, family}, {2, "Regular"}, {3, family + " Regular"}, {4, family}, {5, "Version 1.0"}, {6, ps}};
  std::vector<uint8_t> strs, name;
  be16(name, 0);
  be16(name, (uint16_t)names.size());
  be16(name, (uint16_t)(6 + 12 * names.size()));
  for (auto& n : names) {
    std::vector<uint8_t> s;
    utf16be(s, n.second);
    be16(name, 3);
    be16(name, 1);
    be16(name, 0x409);
    be16(name, (uint16_t)n.first);
    be16(name, (uint16_t)s.size());
    be16(name, (uint16_t)strs.size());
    strs.insert(strs.end(), s.begin(), s.end());
  }
  name.insert(name.end(), strs.begin(), strs.end());

  std::vector<uint8_t> post;
  be32(post, 0x00030000);
  be32(post, 0);
  be16(post, (uint16_t)(int16_t)-100);
  be16(post, 50);
  for (int k = 0; k < 5; k++) be32(post, 0);

  // assemble, tables sorted by tag
  std::vector<std::pair<std::string, std::vector<uint8_t>*>> T = {
      {"OS/2", &os2}, {"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea},
      {"hmtx", &hmtx}, {"loca", &loca}, {"maxp", &maxp}, {"name", &name}, {"post", &post}};
  std::vector<uint8_t> out;
  be32(out, 0x00010000);
  be16(out, (uint16_t)T.size());
  be16(out, 128);
  be16(out, 3);
  be16(out, (uint16_t)(T.size() * 16 - 128));
  uint32_t off = 12 + 16 * (uint32_t)T.size();
  size_t headOff = 0;
  for (auto& t : T) {
    for (char c : t.first) out.push_back((uint8_t)c);
    be32(out, checksum(*t.second));
    be32(out, off);
    be32(out, (uint32_t)t.second->size());
    if (t.first == "head") headOff = off;
    off += ((uint32_t)t.second->size() + 3) & ~3u;
  }
  for (auto& t : T) {
    out.insert(out.end(), t.second->begin(), t.second->end());
    while (out.size() % 4) out.push_back(0);
  }
  uint32_t adj = 0xB1B0AFBAu - checksum(out);
  out[headOff + 8] = (uint8_t)(adj >> 24);
  out[headOff + 9] = (uint8_t)(adj >> 16);
  out[headOff + 10] = (uint8_t)(adj >> 8);
  out[headOff + 11] = (uint8_t)adj;
  return out;
}

// ---------------------------------------------------------------- building
bool buildFont(const FontProject& p, std::string& ttfPath, std::string& err) {
  const std::vector<uint32_t>& cs = fontMakerChars();
  const float k = 1100.f / kGlyphCell;  // cell height = 850 above the baseline + 250 below
  struct Traced {
    uint32_t cp;
    std::vector<std::vector<std::pair<int, int>>> c;
    int x0, x1;
  };
  std::vector<Traced> traced;
  for (size_t i = 0; i < cs.size() && i < p.ink.size(); i++) {
    if (p.ink[i].empty()) continue;
    auto loops = traceBitmap(p.ink[i], kGlyphCell, kGlyphCell);
    if (loops.empty()) continue;
    Traced t;
    t.cp = cs[i];
    t.x0 = 32767;
    t.x1 = -32768;
    for (auto& L : loops) {
      std::vector<std::pair<int, int>> c;
      for (auto& q : L) {
        int x = (int)std::lround(q.first * k), y = (int)std::lround((kGlyphBaseline - q.second) * k);
        if (!c.empty() && c.back().first == x && c.back().second == y) continue;
        c.push_back({x, y});
        t.x0 = std::min(t.x0, x);
        t.x1 = std::max(t.x1, x);
      }
      if (c.size() >= 3) t.c.push_back(c);
    }
    // Winding: outer contours clockwise, holes counter-clockwise (by nesting depth).
    auto signedArea = [](const std::vector<std::pair<int, int>>& c) {
      long long s = 0;
      for (size_t a = 0; a < c.size(); a++) {
        auto& P = c[a];
        auto& Q = c[(a + 1) % c.size()];
        s += (long long)P.first * Q.second - (long long)Q.first * P.second;
      }
      return s;
    };
    auto inside = [](const std::vector<std::pair<int, int>>& c, double x, double y) {
      bool in = false;
      for (size_t a = 0, b = c.size() - 1; a < c.size(); b = a++) {
        double xa = c[a].first, ya = c[a].second, xb = c[b].first, yb = c[b].second;
        if ((ya > y) != (yb > y) && x < (xb - xa) * (y - ya) / (yb - ya) + xa) in = !in;
      }
      return in;
    };
    for (size_t a = 0; a < t.c.size(); a++) {
      int depth = 0;
      double x = t.c[a][0].first + 0.25, y = t.c[a][0].second + 0.25;
      for (size_t b = 0; b < t.c.size(); b++)
        if (b != a && inside(t.c[b], x, y)) depth++;
      bool wantCw = depth % 2 == 0;
      bool isCw = signedArea(t.c[a]) < 0;
      if (wantCw != isCw) std::reverse(t.c[a].begin(), t.c[a].end());
    }
    if (!t.c.empty()) traced.push_back(t);
  }
  if (traced.empty()) {
    err = "draw at least one character first";
    return false;
  }
  // Thai marks sit over the letter before them: centre them half an average consonant to the left.
  int consonantAdv = 0, nCons = 0;
  for (auto& t : traced)
    if (t.cp >= 0x0E01 && t.cp <= 0x0E2E) consonantAdv += (t.x1 - t.x0) + 120, nCons++;
  int markShift = nCons ? consonantAdv / nCons / 2 : 280;

  std::vector<FmGlyph> G;
  FmGlyph notdef;  // a hollow box
  notdef.advance = 600;
  notdef.contours = {{{60, 0}, {60, 700}, {540, 700}, {540, 0}}, {{110, 50}, {490, 50}, {490, 650}, {110, 650}}};
  G.push_back(notdef);
  FmGlyph space;
  space.cp = 0x20;
  space.advance = 280;
  G.push_back(space);
  std::sort(traced.begin(), traced.end(), [](const Traced& a, const Traced& b) { return a.cp < b.cp; });
  for (auto& t : traced) {
    if (t.cp == 0x20) continue;
    FmGlyph g;
    g.cp = t.cp;
    int dx;
    if (isThaiMark(t.cp)) {
      dx = -markShift - (t.x0 + t.x1) / 2;
      g.advance = 0;
    } else {
      dx = 60 - t.x0;
      g.advance = (t.x1 - t.x0) + 120;
    }
    for (auto& c : t.c) {
      std::vector<std::pair<int, int>> m;
      for (auto& q : c) m.push_back({q.first + dx, q.second});
      g.contours.push_back(m);
    }
    G.push_back(g);
  }
  std::vector<uint8_t> ttf = writeTrueType(p.name, G);
  std::string dir = userFontDir();
  if (!mkdirs(dir)) {
    err = "cannot create " + dir;
    return false;
  }
  ttfPath = dir + "\\" + safeName(p.name) + "_" + std::to_string((long long)time(nullptr)) + ".ttf";
  FILE* f = ufopen(ttfPath, "wb");
  if (!f || fwrite(ttf.data(), 1, ttf.size(), f) != ttf.size()) {
    if (f) fclose(f);
    err = "cannot write " + ttfPath;
    return false;
  }
  fclose(f);
  return true;
}

bool installFontForUser(const std::string& ttfPath, const std::string& family, std::string& err) {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    err = "LOCALAPPDATA not set";
    return false;
  }
  std::string dir = wideToUtf8(buf) + "\\Microsoft\\Windows\\Fonts";
  if (!mkdirs(dir)) {
    err = "cannot create " + dir;
    return false;
  }
  std::string dest = dir + "\\" + fileName(ttfPath);
  if (!fileExists(dest) && !copyFileU(ttfPath, dest)) {
    err = "cannot copy the font to " + dir;
    return false;
  }
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    err = "cannot register the font";
    return false;
  }
  std::wstring valName = utf8ToWide(family + " (TrueType)"), val = utf8ToWide(dest);
  LONG rc = RegSetValueExW(key, valName.c_str(), 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS) {
    err = "cannot register the font";
    return false;
  }
  AddFontResourceW(val.c_str());
  DWORD_PTR res = 0;
  SendMessageTimeoutW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0, SMTO_ABORTIFHUNG, 1000, &res);
  return true;
}
