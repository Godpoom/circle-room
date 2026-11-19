#include "layer_content.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvg.h"
#include "../third_party/nanosvg/nanosvgrast.h"
#include "platform_win.h"
#include "updater.h"  // sha256Hex

namespace {

const size_t kMaxContentBytes = 4u << 20;
const int kMaxTextSide = 4096;
const size_t kMaxTextChars = 4000;

bool endsWithI(const std::string& s, const char* ext) {
  size_t n = strlen(ext);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)ext[i])) return false;
  return true;
}

std::string escapeLine(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') continue;
    else o += c;
  }
  return o;
}

std::string unescapeLine(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      o += n == 'n' ? '\n' : n;
    } else {
      o += s[i];
    }
  }
  return o;
}

}  // namespace

// ---------------------------------------------------------------- colours
std::string colorHex(uint32_t c) {
  char b[16];
  if ((c & 0xFF) == 0xFF) snprintf(b, sizeof(b), "#%06X", (unsigned)(c >> 8));
  else snprintf(b, sizeof(b), "#%08X", (unsigned)c);
  return b;
}

bool parseColorHex(const std::string& s0, uint32_t& out) {
  std::string s = s0;
  if (!s.empty() && s[0] == '#') s.erase(0, 1);
  if (s.size() != 6 && s.size() != 8) return false;
  for (char c : s)
    if (!isxdigit((unsigned char)c)) return false;
  unsigned long v = strtoul(s.c_str(), nullptr, 16);
  out = s.size() == 6 ? (uint32_t)((v << 8) | 0xFF) : (uint32_t)v;
  return true;
}

// ---------------------------------------------------------------- content files
std::string layerContentDir(const std::string& photo) { return dirOf(photo) + "\\.circleroom_layers"; }
bool isTextContent(const std::string& path) { return endsWithI(path, ".crtext"); }
bool isVectorContent(const std::string& path) { return endsWithI(path, ".svg"); }

bool writeLayerContent(const std::string& photo, const std::string& data, const char* ext, std::string& path, std::string& err) {
  if (data.size() > kMaxContentBytes) {
    err = "content too large (4 MB at most)";
    return false;
  }
  std::string dir = layerContentDir(photo);
  if (!isDirectory(dir)) {
    if (!ensureDir(dir)) {
      err = "cannot create " + dir;
      return false;
    }
    setHiddenU(dir);
  }
  path = dir + "\\" + sha256Hex(data.data(), data.size()).substr(0, 24) + "." + ext;
  if (fileExists(path)) return true;  // same content: same file
  if (!writeTextFile(path, data)) {
    err = "cannot write " + path;
    return false;
  }
  return true;
}

bool readLayerContent(const std::string& path, std::string& data, std::string& err) {
  unsigned long long size = 0;
  long long mt = 0;
  if (!fileInfo(path, size, mt)) {
    err = "content file missing: " + fileName(path);
    return false;
  }
  if (size > kMaxContentBytes) {
    err = "content file too large";
    return false;
  }
  if (!readTextFile(path, data)) {
    err = "cannot read " + fileName(path);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- text spec
std::string serializeText(const TextSpec& t) {
  std::ostringstream o;
  o << "crtext=1\n";
  o << "font=" << escapeLine(t.font) << "\n";
  o << "color=" << colorHex(t.color) << "\n";
  o << "bold=" << (t.bold ? 1 : 0) << "\nitalic=" << (t.italic ? 1 : 0) << "\nalign=" << t.align << "\n";
  o << "spacing=" << t.spacing << "\noutline=" << t.outline << "\noutlineColor=" << colorHex(t.outlineColor) << "\n";
  o << "text=" << escapeLine(t.text) << "\n";
  return o.str();
}

bool parseText(const std::string& data, TextSpec& t) {
  t = TextSpec();
  std::stringstream ss(data);
  std::string line;
  bool magic = false;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "crtext") magic = true;
    else if (k == "font") t.font = unescapeLine(v).substr(0, 120);
    else if (k == "color") parseColorHex(v, t.color);
    else if (k == "outlineColor") parseColorHex(v, t.outlineColor);
    else if (k == "bold") t.bold = atoi(v.c_str()) ? 1 : 0;
    else if (k == "italic") t.italic = atoi(v.c_str()) ? 1 : 0;
    else if (k == "align") t.align = std::clamp(atoi(v.c_str()), 0, 2);
    else if (k == "spacing") t.spacing = (float)atof(v.c_str());
    else if (k == "outline") t.outline = (float)atof(v.c_str());
    else if (k == "text") t.text = unescapeLine(v);
  }
  if (!std::isfinite(t.spacing)) t.spacing = 100.f;
  if (!std::isfinite(t.outline)) t.outline = 0.f;
  t.spacing = std::clamp(t.spacing, 50.f, 300.f);
  t.outline = std::clamp(t.outline, 0.f, 30.f);
  if (t.text.size() > kMaxTextChars * 3) t.text.resize(kMaxTextChars * 3);
  return magic;
}

// ---------------------------------------------------------------- font list samples
bool renderFontSample(const std::string& family, const std::string& sample, int h, int maxW, std::vector<uint8_t>& alpha,
                      int& w) {
  initFonts();
  h = std::clamp(h, 8, 256);
  maxW = std::clamp(maxW, 16, 2048);
  std::wstring text = utf8ToWide(sample), face = utf8ToWide(family);
  if (text.empty()) return false;
  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) return false;
  // Size the font so ascent + descent fills ~94% of the row (Thai marks need the room).
  HFONT font = CreateFontW(-h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.substr(0, 31).c_str());
  HGDIOBJ oldFont = SelectObject(dc, font);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  if (tm.tmHeight > 0 && tm.tmHeight != (int)(h * 0.94f)) {
    int px = std::max(6, (int)std::lround(h * 0.94f * h / tm.tmHeight));
    SelectObject(dc, oldFont);
    DeleteObject(font);
    font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.substr(0, 31).c_str());
    SelectObject(dc, font);
    GetTextMetricsW(dc, &tm);
  }
  RECT r = {0, 0, 0, 0};
  DrawTextW(dc, text.c_str(), (int)text.size(), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
  w = std::clamp((int)(r.right - r.left) + 4, 4, maxW);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = bmp && bits;
  if (ok) {
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT dr = {1, (h - tm.tmHeight) / 2, w, h};
    DrawTextW(dc, text.c_str(), (int)text.size(), &dr, DT_SINGLELINE | DT_NOPREFIX | DT_LEFT | DT_TOP);
    GdiFlush();
    alpha.resize((size_t)w * h);
    const uint8_t* s = (const uint8_t*)bits;
    for (size_t i = 0; i < alpha.size(); i++) alpha[i] = std::max({s[i * 4], s[i * 4 + 1], s[i * 4 + 2]});
    SelectObject(dc, oldBmp);
  }
  if (bmp) DeleteObject(bmp);
  SelectObject(dc, oldFont);
  DeleteObject(font);
  DeleteDC(dc);
  return ok;
}

// ---------------------------------------------------------------- text rendering (GDI: shapes Thai properly)
// The glyphs as coverage (0..255), w x h with room for marks and the outline; R = outline radius.
static bool drawInk(const TextSpec& t, std::vector<uint8_t>& ta, int& w, int& h, float& R, std::string& err) {
  initFonts();
  std::vector<std::wstring> lines;
  {
    std::wstring all = utf8ToWide(t.text.empty() ? std::string(" ") : t.text);
    if (all.size() > kMaxTextChars) all.resize(kMaxTextChars);
    size_t s = 0;
    for (size_t i = 0; i <= all.size(); i++)
      if (i == all.size() || all[i] == L'\n') {
        std::wstring ln = all.substr(s, i - s);
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        lines.push_back(ln.empty() ? L" " : ln);
        s = i + 1;
      }
    if (lines.size() > 200) lines.resize(200);
  }
  std::wstring face = utf8ToWide(t.font);
  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) {
    err = "GDI unavailable";
    return false;
  }
  int px = 220;  // internal font size; the layer's scale sets the size on the photo
  HFONT font = nullptr;
  int lineH = 0, maxW = 0, pad = 0;
  std::vector<int> lw;
  for (int attempt = 0; attempt < 6; attempt++) {
    if (font) DeleteObject(font);
    font = CreateFontW(-px, 0, 0, 0, t.bold ? FW_BOLD : FW_NORMAL, t.italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       face.substr(0, 31).c_str());
    SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    lineH = (int)std::lround((tm.tmHeight + tm.tmExternalLeading) * t.spacing / 100.f);
    lw.clear();
    maxW = 1;
    for (const std::wstring& ln : lines) {
      RECT r = {0, 0, 0, 0};
      DrawTextW(dc, ln.c_str(), (int)ln.size(), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
      int wl = r.right - r.left + (t.italic ? px / 4 : 0);
      lw.push_back(wl);
      maxW = std::max(maxW, wl);
    }
    pad = (int)std::ceil(px * 0.35f + t.outline * px / 100.f) + 4;  // Thai marks above / below, outline
    w = maxW + 2 * pad;
    h = lineH * (int)(lines.size() - 1) + tm.tmHeight + 2 * pad;
    if (w <= kMaxTextSide && h <= kMaxTextSide) break;
    px = std::max(12, (int)(px * 0.95f * kMaxTextSide / std::max(w, h)));
  }
  w = std::min(w, kMaxTextSide);
  h = std::min(h, kMaxTextSide);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    if (font) DeleteObject(font);
    DeleteDC(dc);
    err = "out of memory for text";
    return false;
  }
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  SelectObject(dc, font);
  std::memset(bits, 0, (size_t)w * h * 4);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  for (size_t i = 0; i < lines.size(); i++) {
    int x = pad + (t.align == 0 ? 0 : t.align == 1 ? (maxW - lw[i]) / 2 : maxW - lw[i]);
    RECT r = {x, pad + lineH * (int)i, w, h};
    DrawTextW(dc, lines[i].c_str(), (int)lines[i].size(), &r, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP | DT_LEFT | DT_TOP);
  }
  GdiFlush();
  ta.resize((size_t)w * h);
  const uint8_t* src = (const uint8_t*)bits;
  for (size_t i = 0; i < ta.size(); i++) ta[i] = std::max({src[i * 4], src[i * 4 + 1], src[i * 4 + 2]});
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteObject(font);
  DeleteDC(dc);
  R = t.outline * px / 100.f;
  return true;
}

static const int kTrimMargin = 6;

// Width / height of renderText's result without the outline pass or colour: the ink box grown by
// the outline exactly as the chamfer distance grows it along rows and columns, plus the margin.
bool measureText(const TextSpec& t, float& aspect) {
  std::vector<uint8_t> ta;
  int w = 0, h = 0;
  float R = 0;
  std::string err;
  if (!drawInk(t, ta, w, h, R, err)) return false;
  float op = (t.color & 0xFF) / 255.f;
  int x0 = w, y0 = h, x1 = -1, y1 = -1, sx0 = w, sy0 = h, sx1 = -1, sy1 = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t v = ta[(size_t)y * w + x];
      if (v && std::lround(v / 255.f * op * 255.f) > 0) {
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
      }
      if (v >= 128) sx0 = std::min(sx0, x), sx1 = std::max(sx1, x), sy0 = std::min(sy0, y), sy1 = std::max(sy1, y);
    }
  if (R > 0.5f && sx1 >= 0) {
    // oa > 0 where d < R + 0.5 - 1/510; along a row d is the plain pixel count
    int k = (int)std::ceil(R + 0.5f - 1.f / 510.f) - 1;
    x0 = std::min(x0, std::max(0, sx0 - k)), x1 = std::max(x1, std::min(w - 1, sx1 + k));
    y0 = std::min(y0, std::max(0, sy0 - k)), y1 = std::max(y1, std::min(h - 1, sy1 + k));
  }
  if (x1 >= 0) {
    x0 = std::max(0, x0 - kTrimMargin), y0 = std::max(0, y0 - kTrimMargin);
    x1 = std::min(w - 1, x1 + kTrimMargin), y1 = std::min(h - 1, y1 + kTrimMargin);
    w = x1 - x0 + 1;
    h = y1 - y0 + 1;
  }
  aspect = (float)w / std::max(1, h);
  return true;
}

bool renderText(const TextSpec& t, std::vector<uint8_t>& rgb, std::vector<uint8_t>& alpha, int& w, int& h, std::string& err) {
  std::vector<uint8_t> ta;
  float R = 0;
  if (!drawInk(t, ta, w, h, R, err)) return false;

  // Outline: distance to the nearest ink (two-pass chamfer), anti-aliased at the edge.
  std::vector<uint8_t> oa;
  if (R > 0.5f) {
    const float INF = 1e9f;
    std::vector<float> d((size_t)w * h);
    for (size_t i = 0; i < d.size(); i++) d[i] = ta[i] >= 128 ? 0.f : INF;
    auto at = [&](int x, int y) -> float& { return d[(size_t)y * w + x]; };
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        float v = at(x, y);
        if (x > 0) v = std::min(v, at(x - 1, y) + 1.f);
        if (y > 0) v = std::min(v, at(x, y - 1) + 1.f);
        if (x > 0 && y > 0) v = std::min(v, at(x - 1, y - 1) + 1.4142f);
        if (x + 1 < w && y > 0) v = std::min(v, at(x + 1, y - 1) + 1.4142f);
        at(x, y) = v;
      }
    for (int y = h - 1; y >= 0; y--)
      for (int x = w - 1; x >= 0; x--) {
        float v = at(x, y);
        if (x + 1 < w) v = std::min(v, at(x + 1, y) + 1.f);
        if (y + 1 < h) v = std::min(v, at(x, y + 1) + 1.f);
        if (x + 1 < w && y + 1 < h) v = std::min(v, at(x + 1, y + 1) + 1.4142f);
        if (x > 0 && y + 1 < h) v = std::min(v, at(x - 1, y + 1) + 1.4142f);
        at(x, y) = v;
      }
    oa.resize(d.size());
    for (size_t i = 0; i < d.size(); i++) oa[i] = (uint8_t)std::lround(std::clamp(R + 0.5f - d[i], 0.f, 1.f) * 255.f);
  }
  uint8_t tc[3] = {(uint8_t)(t.color >> 24), (uint8_t)(t.color >> 16), (uint8_t)(t.color >> 8)};
  uint8_t oc[3] = {(uint8_t)(t.outlineColor >> 24), (uint8_t)(t.outlineColor >> 16), (uint8_t)(t.outlineColor >> 8)};
  float textOpacity = (t.color & 0xFF) / 255.f;
  rgb.resize((size_t)w * h * 3);
  alpha.resize((size_t)w * h);
  for (size_t i = 0; i < ta.size(); i++) {
    float a = ta[i] / 255.f;
    float o = oa.empty() ? 0.f : oa[i] / 255.f;
    for (int c = 0; c < 3; c++) rgb[i * 3 + c] = (uint8_t)std::lround(oc[c] + (tc[c] - oc[c]) * (oa.empty() ? 1.f : a));
    alpha[i] = (uint8_t)std::lround(std::max(a * textOpacity, o) * 255.f);
  }
  // Trim to the ink (plus a small margin): the layer's size and centre then match the visible text.
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (alpha[(size_t)y * w + x]) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
      }
  if (x1 >= 0) {
    const int m = kTrimMargin;
    x0 = std::max(0, x0 - m);
    y0 = std::max(0, y0 - m);
    x1 = std::min(w - 1, x1 + m);
    y1 = std::min(h - 1, y1 + m);
    int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    std::vector<uint8_t> r2((size_t)nw * nh * 3), a2((size_t)nw * nh);
    for (int y = 0; y < nh; y++) {
      std::memcpy(&r2[(size_t)y * nw * 3], &rgb[((size_t)(y0 + y) * w + x0) * 3], (size_t)nw * 3);
      std::memcpy(&a2[(size_t)y * nw], &alpha[(size_t)(y0 + y) * w + x0], (size_t)nw);
    }
    rgb.swap(r2);
    alpha.swap(a2);
    w = nw;
    h = nh;
  }
  return true;
}

// ---------------------------------------------------------------- SVG
bool renderSvg(const std::string& svg, int maxDim, std::vector<uint8_t>& rgb, std::vector<uint8_t>& alpha, int& w, int& h,
               std::string& err) {
  if (svg.size() > kMaxContentBytes) {
    err = "SVG too large";
    return false;
  }
  std::vector<char> buf(svg.begin(), svg.end());
  buf.push_back(0);
  NSVGimage* im = nsvgParse(buf.data(), "px", 96.f);
  if (!im || im->width <= 0 || im->height <= 0 || !std::isfinite(im->width) || !std::isfinite(im->height)) {
    if (im) nsvgDelete(im);
    err = "not a usable SVG (it needs a width and height or a viewBox)";
    return false;
  }
  maxDim = std::clamp(maxDim, 16, 4096);
  float scale = maxDim / std::max(im->width, im->height);
  w = std::max(1, (int)std::ceil(im->width * scale));
  h = std::max(1, (int)std::ceil(im->height * scale));
  std::vector<uint8_t> rgba((size_t)w * h * 4, 0);
  NSVGrasterizer* rast = nsvgCreateRasterizer();
  if (!rast) {
    nsvgDelete(im);
    err = "out of memory for SVG";
    return false;
  }
  nsvgRasterize(rast, im, 0, 0, scale, rgba.data(), w, h, w * 4);
  nsvgDeleteRasterizer(rast);
  nsvgDelete(im);
  rgb.resize((size_t)w * h * 3);
  alpha.resize((size_t)w * h);
  for (size_t i = 0; i < alpha.size(); i++) {
    std::memcpy(&rgb[i * 3], &rgba[i * 4], 3);
    alpha[i] = rgba[i * 4 + 3];
  }
  return true;
}

// ---------------------------------------------------------------- fonts
namespace {
std::mutex g_fontM;
std::vector<FontEntry> g_fonts;
std::set<std::wstring> g_loadedFiles;
bool g_fontsInit = false;

int CALLBACK enumFamily(const LOGFONTW* lf, const TEXTMETRICW*, DWORD type, LPARAM lp) {
  auto* out = (std::vector<FontEntry>*)lp;
  if (lf->lfFaceName[0] == L'@' || !(type & TRUETYPE_FONTTYPE)) return 1;  // vertical CJK variants, bitmap fonts
  std::string fam = wideToUtf8(lf->lfFaceName);
  for (FontEntry& e : *out)
    if (e.family == fam) {
      e.thai |= lf->lfCharSet == THAI_CHARSET;
      return 1;
    }
  FontEntry e;
  e.family = fam;
  e.thai = lf->lfCharSet == THAI_CHARSET;
  out->push_back(e);
  return 1;
}

void loadDir(const std::string& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileW(utf8ToWide(dir + "\\*").c_str(), &fd);
  if (f == INVALID_HANDLE_VALUE) return;
  do {
    std::string name = wideToUtf8(fd.cFileName);
    if (name == "." || name == "..") continue;
    std::string p = dir + "\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) loadDir(p);
    } else if (endsWithI(name, ".ttf") || endsWithI(name, ".otf")) {
      std::wstring w = utf8ToWide(p);
      if (!g_loadedFiles.count(w) && AddFontResourceExW(w.c_str(), FR_PRIVATE, nullptr) > 0) g_loadedFiles.insert(w);
    }
  } while (FindNextFileW(f, &fd));
  FindClose(f);
}

void enumerate() {
  std::vector<FontEntry> list;
  HDC dc = GetDC(nullptr);
  LOGFONTW lf = {};
  lf.lfCharSet = DEFAULT_CHARSET;
  EnumFontFamiliesExW(dc, &lf, enumFamily, (LPARAM)&list, 0);
  // DEFAULT_CHARSET lists each family once; a second pass marks the ones with Thai letters.
  lf.lfCharSet = THAI_CHARSET;
  EnumFontFamiliesExW(dc, &lf, enumFamily, (LPARAM)&list, 0);
  ReleaseDC(nullptr, dc);
  std::sort(list.begin(), list.end(), [](const FontEntry& a, const FontEntry& b) {
    return _stricmp(a.family.c_str(), b.family.c_str()) < 0;
  });
  g_fonts.swap(list);
}
}  // namespace

std::string userFontDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::string base = (n > 0 && n < MAX_PATH) ? wideToUtf8(buf) : exeDir();
  return base + "\\CircleRoom\\fonts";
}

void initFonts() {
  std::lock_guard<std::mutex> lk(g_fontM);
  if (g_fontsInit) return;
  g_fontsInit = true;
  loadDir(exeDir() + "\\fonts");
  loadDir(userFontDir());
  enumerate();
}

void reloadFonts() {
  std::lock_guard<std::mutex> lk(g_fontM);
  g_fontsInit = true;
  loadDir(exeDir() + "\\fonts");
  loadDir(userFontDir());
  enumerate();
}

bool loadPrivateFont(const std::string& file) {
  std::lock_guard<std::mutex> lk(g_fontM);
  std::wstring w = utf8ToWide(file);
  if (!g_loadedFiles.count(w)) {
    if (AddFontResourceExW(w.c_str(), FR_PRIVATE, nullptr) <= 0) return false;
    g_loadedFiles.insert(w);
  }
  enumerate();
  return true;
}

const std::vector<FontEntry>& fontList() {
  initFonts();
  return g_fonts;
}

bool fontAvailable(const std::string& family) {
  for (const FontEntry& e : fontList())
    if (_stricmp(e.family.c_str(), family.c_str()) == 0) return true;
  return false;
}
