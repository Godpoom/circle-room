#include "mcp_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>

#include "ai/cache.h"
#include "ai/cull.h"
#include "ai/highlight.h"
#include "ai/ort.h"
#include "ai/segment.h"
#include "ai/style_match.h"
#include "ai/text_query.h"
#include "agent_log.h"
#include "heal.h"
#include "i18n.h"
#include "image_io.h"
#include "layers.h"
#include "mcp_json.h"
#include "params.h"
#include "platform_win.h"
#include "renderer.h"
#include "ui_cull.h"

namespace {

// ---------------------------------------------------------------- small helpers
std::string b64(const std::vector<uint8_t>& d) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((d.size() + 2) / 3 * 4);
  for (size_t i = 0; i < d.size(); i += 3) {
    uint32_t v = d[i] << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
    o += T[v >> 18 & 63];
    o += T[v >> 12 & 63];
    o += i + 1 < d.size() ? T[v >> 6 & 63] : '=';
    o += i + 2 < d.size() ? T[v & 63] : '=';
  }
  return o;
}

std::string fmt(const char* f, double v) {
  char b[64];
  snprintf(b, sizeof(b), f, v);
  return b;
}

// Tool results: text plus optional JPEG images the agent can look at.
struct Result {
  std::string text;
  std::vector<std::vector<uint8_t>> images;
  bool error = false;
};

Result fail(const std::string& why) {
  Result r;
  r.text = why;
  r.error = true;
  return r;
}

struct Adj {
  const char* key;
  float lo, hi;
  const char* what;
};
// Everything set_adjustments understands (the same keys a .pedit file uses).
const Adj kAdj[] = {
    {"exposure", -5, 5, "brightness in EV stops"},
    {"contrast", -100, 100, "global contrast"},
    {"highlights", -100, 100, "bright areas (negative recovers detail)"},
    {"shadows", -100, 100, "dark areas (positive lifts them)"},
    {"whites", -100, 100, "white point"},
    {"blacks", -100, 100, "black point"},
    {"temp", -100, 100, "white balance: negative cooler/blue, positive warmer/yellow"},
    {"tint", -100, 100, "white balance: negative green, positive magenta"},
    {"vibrance", -100, 100, "saturation that protects skin and already-vivid colours"},
    {"saturation", -100, 100, "all colours (-100 = black & white)"},
    {"texture", -100, 100, "fine detail"},
    {"clarity", -100, 100, "local mid-tone contrast"},
    {"dehaze", -100, 100, "removes (or adds) haze"},
    {"sharpAmount", 0, 150, "sharpening amount"},
    {"sharpRadius", 0.5f, 3, "sharpening radius"},
    {"sharpMasking", 0, 100, "sharpen edges only (higher = fewer flat areas)"},
    {"nrLum", 0, 100, "luminance noise reduction"},
    {"nrColor", 0, 100, "colour noise reduction"},
    {"vigAmount", -100, 100, "vignette (negative darkens corners)"},
    {"vigMidpoint", 0, 100, "vignette size"},
    {"vigFeather", 0, 100, "vignette softness"},
    {"vigRoundness", -100, 100, "vignette shape"},
    {"grainAmount", 0, 100, "film grain"},
    {"grainSize", 0, 100, "grain size"},
    {"grainRoughness", 0, 100, "grain roughness"},
    {"gradeBlending", 0, 100, "colour grading: how far the shadow/highlight tints overlap"},
    {"gradeBalance", -100, 100, "colour grading: shadows vs highlights balance"},
};
const char* kBands[kHslBands] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
const char* kGrades[GRADE_COUNT] = {"shadows", "midtones", "highlights", "global"};
const char* kRegions[ai::REG_COUNT] = {"person", "sky",     "road",   "building", "trees", "grass", "water",
                                       "mountain", "ground", "vehicle", "animal", "walls", "food", "face"};
const char* kReasons[ai::R_COUNT_BITS] = {"blurry",    "face blurred",     "eyes closed", "overexposed", "underexposed",
                                          "noisy",     "tilted",           "subject cut by the edge", "off balance",
                                          "good composition", "sharp", "good light", "duplicate", "like references"};
const char* kBlends[BLEND_COUNT] = {"normal", "multiply", "screen", "overlay", "soft-light", "darken", "lighten"};

int regionIndex(const std::string& n) {
  for (int i = 0; i < ai::REG_COUNT; i++)
    if (n == kRegions[i]) return i;
  return -1;
}

// ---------------------------------------------------------------- the photo being edited
struct Session {
  Renderer R, aiR;  // R holds the photo; aiR the small images the AI tools solve on
  bool aiInit = false;
  std::string path;
  EditParams p;
  std::vector<EditParams> undo;
  HealStore heal;
  LayerStore layers;
  ai::SegResult seg;
  std::vector<float> guide;
  int gw = 0, gh = 0;
  bool segDone = false, segOk = false, masksDirty = true, recLoaded = false;
  AutoModels autoModels;

  bool loaded() const { return !path.empty(); }

  bool open(const std::string& file, std::string& err) {
    ImageData img;
    if (!loadImage(file, R.maxTextureSize(), img, err)) return false;
    if (!R.upload(img, err)) return false;
    path = file;
    p = EditParams();
    undo.clear();
    std::string sc;
    if (readSidecar(path, sc)) deserializeParams(sc, p, true);
    heal.clear(R);
    heal.parse(sc);
    layers.clear(R);
    layers.parseStrokes(sc);
    seg = ai::SegResult();
    guide.clear();
    segDone = segOk = false;
    masksDirty = true;
    recLoaded = false;
    R.clearMask();
    R.clearRecover();
    return true;
  }

  void save() {
    writeSidecar(path, isDefault(p) ? std::string() : serializeParams(p, true) + layers.serializeStrokes(p) + heal.serialize(p));
  }
  void checkpoint() {
    undo.push_back(p);
    if (undo.size() > 100) undo.erase(undo.begin());
  }

  bool segment(std::string& err) {
    if (segDone) return segOk;
    segDone = true;
    segOk = ai::segmentFile(path, seg, guide, gw, gh, err);
    return segOk;
  }

  // Everything the render needs that is not in EditParams: AI masks, highlight overlay,
  // magic-eraser patches and layer photos (blocking: the agent waits for its result).
  void prepare() {
    std::string err;
    if (anyLayer(p) && masksDirty && segment(err)) {
      std::vector<uint8_t> mask;
      ai::buildMasks(seg, guide, gw, gh, p.layers, mask);
      R.setMask(mask.data(), gw, gh);
      masksDirty = false;
    }
    if (p.recover > 0 && !recLoaded) {
      ai::Recovery rec;
      if (ai::recoverFile(path, segOk ? &seg : nullptr, true, rec, err) && rec.valid()) R.setRecover(rec.rgba.data(), rec.w, rec.h);
      recLoaded = true;
    }
    if (p.heals > 0) heal.sync(p, R, path, true);
    if (anyImageLayer(p)) {
      layers.sync(p, R, true);
      layers.sync(p, R, false);
    }
  }

  Geometry geom() const { return computeGeometry(p, R.srcW(), R.srcH(), R.orientation(), false); }

  // Rendered view (crop applied), long edge <= size.
  void render(const EditParams& q, int size, std::vector<uint8_t>& rgb, int& w, int& h) {
    Geometry g = computeGeometry(q, R.srcW(), R.srcH(), R.orientation(), false);
    float k = std::min(1.f, (float)size / std::max(g.outW, g.outH));
    w = std::max(1, (int)std::lround(g.outW * k));
    h = std::max(1, (int)std::lround(g.outH * k));
    std::vector<uint8_t> rgba;
    R.renderSmall(q, w, h, rgba);
    rgb.resize((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) std::memcpy(&rgb[i * 3], &rgba[i * 4], 3);
  }
};

std::vector<uint8_t> jpeg(const std::vector<uint8_t>& rgb, int w, int h, int q = 85) {
  std::vector<uint8_t> out;
  encodeJpegMem(rgb.data(), w, h, q, out);
  return out;
}

// Plain-language numbers the agent can reason about.
std::string stats(const std::vector<uint8_t>& rgb, int w, int h) {
  size_t n = (size_t)w * h, hi = 0, lo = 0;
  double sum = 0, r = 0, b = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t* q = &rgb[i * 3];
    int Y = (q[0] * 54 + q[1] * 183 + q[2] * 19) >> 8;
    sum += Y;
    r += q[0];
    b += q[2];
    hi += std::max({q[0], q[1], q[2]}) >= 252;
    lo += Y <= 4;
  }
  std::string s = "brightness " + fmt("%.0f", sum / n) + "/255, clipped highlights " + fmt("%.1f", 100.0 * hi / n) +
                  "%, crushed shadows " + fmt("%.1f", 100.0 * lo / n) + "%, colour balance " +
                  (r > b * 1.06 ? "warm" : b > r * 1.06 ? "cool" : "neutral");
  return s;
}

// What differs from a fresh photo, in the same words the tools use.
std::string describeEdits(const EditParams& p) {
  EditParams d;
  std::string out;
  std::string a = serializeParams(p, true), b = serializeParams(d, true);
  std::stringstream sa(a);
  std::string line;
  while (std::getline(sa, line))
    if (b.find(line + "\n") == std::string::npos && line.compare(0, 3, "img") != 0) out += "  " + line + "\n";
  return out.empty() ? "  (no edits)\n" : out;
}

// ---------------------------------------------------------------- activity shown in the app
std::map<std::string, std::string> keyValues(const EditParams& p) {
  std::map<std::string, std::string> m;
  std::stringstream ss(serializeParams(p, true));
  std::string line;
  while (std::getline(ss, line)) {
    size_t eq = line.find('=');
    if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return m;
}

// What one tool call changed, sliders one by one and the rest as groups.
std::vector<AgentChange> editChanges(const EditParams& a, const EditParams& b) {
  std::map<std::string, std::string> A = keyValues(a), B = keyValues(b);
  std::vector<AgentChange> out;
  std::vector<std::string> groups;
  auto group = [](const std::string& k) -> std::string {
    if (k.rfind("hsl", 0) == 0) return "hsl";
    if (k.rfind("curve", 0) == 0) return "curve";
    if (k.rfind("grade", 0) == 0) return "grade";
    if (k.rfind("crop", 0) == 0 || k == "straighten" || k == "rotate90" || k == "flipH" || k == "flipV") return "crop";
    if (k.rfind("layer", 0) == 0) return "region";
    if (k.rfind("img", 0) == 0) return "layer";
    return "";
  };
  std::map<std::string, std::string> keys = A;
  keys.insert(B.begin(), B.end());
  for (const auto& kvp : keys) {
    const std::string& k = kvp.first;
    std::string va = A.count(k) ? A[k] : "", vb = B.count(k) ? B[k] : "";
    if (va == vb) continue;
    std::string g = group(k);
    if (g.empty()) out.push_back({k, va, vb});
    else if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
  }
  for (const std::string& g : groups) {
    AgentChange c{g, "", ""};
    if (g == "region") {  // which areas have their own adjustment now
      for (int i = 0; i < kMaxLayers; i++)
        if (b.layers[i].enabled && b.layers[i].region >= 0 && b.layers[i].region < ai::REG_COUNT)
          c.after += (c.after.empty() ? "" : ", ") + std::string(kRegions[b.layers[i].region]);
    } else if (g == "layer") {  // photo layers on top, by file name
      int na = 0;
      for (int i = 0; i < kMaxImgLayers; i++) na += a.img[i].enabled;
      c.before = std::to_string(na);
      for (int i = 0; i < kMaxImgLayers; i++)
        if (b.img[i].enabled) c.after += (c.after.empty() ? "" : ", ") + fileName(b.img[i].path);
    }
    out.push_back(c);
  }
  return out;
}

// Cuts at a character boundary so Thai and other UTF-8 text never ends mid-letter.
std::string clip(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return s.substr(0, n) + "...";
}

// The arguments in one short line ("exposure=0.3, contrast=12"), for the app's activity list.
std::string argSummary(const Json& a) {
  std::string out;
  for (const auto& kv : a.o) {
    if (kv.first == "path") continue;
    std::string v;
    if (kv.second.isNum()) v = fmt("%.3g", kv.second.n);
    else if (kv.second.isStr()) v = clip(kv.second.s.find('\\') != std::string::npos ? fileName(kv.second.s) : kv.second.s, 48);
    else if (kv.second.t == Json::Bool) v = kv.second.b ? "true" : "false";
    else v = clip(json::dump(kv.second), 60);
    out += (out.empty() ? "" : ", ") + kv.first + "=" + v;
  }
  return clip(out, 200);
}

Json schemaObj() {
  Json s = Json::object();
  s.set("type", "object");
  s.set("properties", Json::object());
  return s;
}
void prop(Json& s, const char* name, const char* type, const char* desc) {
  Json p = Json::object();
  p.set("type", type);
  p.set("description", desc);
  s.o[1].second.set(name, p);
}
void required(Json& s, std::initializer_list<const char*> names) {
  Json r = Json::array();
  for (const char* n : names) r.push(n);
  s.set("required", r);
}

}  // namespace

// ---------------------------------------------------------------- tools
namespace {

struct Tool {
  const char* name;
  const char* title;
  const char* desc;
  std::function<Json()> schema;
  std::function<Result(Session&, const Json&)> run;
};

// The view after a change: what the agent looks at to decide the next step.
Result preview(Session& S, const std::string& head, int size = 1024, bool compare = false) {
  Result r;
  S.prepare();
  std::vector<uint8_t> rgb;
  int w, h;
  S.render(S.p, size, rgb, w, h);
  r.text = head + (head.empty() ? "" : "\n") + "Photo: " + fileName(S.path) + " (" + std::to_string(w) + "x" +
           std::to_string(h) + " preview). " + stats(rgb, w, h) + ".";
  if (compare) {
    EditParams before;
    copyGeometry(before, S.p);
    std::vector<uint8_t> b;
    int bw, bh;
    S.render(before, size / 2 + 1, b, bw, bh);
    std::vector<uint8_t> a2;
    int aw, ah;
    S.render(S.p, size / 2 + 1, a2, aw, ah);
    int W = bw + aw + 8, H = std::max(bh, ah);
    std::vector<uint8_t> side((size_t)W * H * 3, 18);
    for (int y = 0; y < bh; y++) std::memcpy(&side[(size_t)y * W * 3], &b[(size_t)y * bw * 3], (size_t)bw * 3);
    for (int y = 0; y < ah; y++) std::memcpy(&side[((size_t)y * W + bw + 8) * 3], &a2[(size_t)y * aw * 3], (size_t)aw * 3);
    r.images.push_back(jpeg(side, W, H));
    r.text += " Image: BEFORE (left) | AFTER (right).";
  } else {
    r.images.push_back(jpeg(rgb, w, h));
  }
  return r;
}

bool ensurePhoto(Session& S, const Json& a, std::string& err) {
  std::string path = a.str("path");
  if (!path.empty() && path != S.path) {
    if (S.open(path, err)) return true;
    err = "Cannot open " + path + ": " + err;
    return false;
  }
  if (!S.loaded()) {
    err = "No photo is open. Call open_photo first (or pass \"path\").";
    return false;
  }
  return true;
}

bool initAi(Session& S, std::string& err) {
  if (!S.aiInit) S.aiInit = S.aiR.init(err);
  return S.aiInit;
}

// Converts a point in view coordinates (0..1 of the shown, cropped image) to the photo's own.
void viewToOriented(const Geometry& g, float x, float y, float& ox, float& oy) { applyM3(g.outToOriented, x, y, ox, oy); }

std::vector<Tool> tools() {
  std::vector<Tool> T;

  T.push_back({"open_photo", "Open a photo",
               "Opens a photo for editing and returns what it looks like now (with any edits already saved for it). "
               "Start here. Supports JPEG, PNG, TIFF, BMP. Edits are non-destructive: they are saved next to the "
               "photo as <photo>.pedit and the original file is never changed.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Absolute path of the photo");
                 required(s, {"path"});
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!S.open(a.str("path"), err)) return fail("Cannot open: " + err);
                 return preview(S, "Opened. Current edits:\n" + describeEdits(S.p));
               }});

  T.push_back({"view", "Look at the photo",
               "Renders the current photo with its edits. Use compare=true to see BEFORE | AFTER side by side.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional: defaults to the open photo)");
                 prop(s, "size", "number", "Long edge of the preview in pixels (256-2048, default 1024)");
                 prop(s, "compare", "boolean", "Show the original next to the edit");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 int size = (int)std::clamp(a.num("size", 1024), 256.0, 2048.0);
                 return preview(S, "", size, a.flag("compare", false));
               }});

  T.push_back({"get_info", "Photo info and current edits",
               "Lists the photo's size, every edit currently applied, local (region) adjustments and photo layers.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 Geometry g = S.geom();
                 std::string t = "Photo: " + S.path + "\nSize after crop: " + std::to_string(g.outW) + "x" +
                                 std::to_string(g.outH) + " px\nEdits:\n" + describeEdits(S.p);
                 t += "Region adjustments:\n";
                 bool any = false;
                 for (int i = 0; i < kMaxLayers; i++) {
                   const LocalLayer& l = S.p.layers[i];
                   if (!l.enabled) continue;
                   any = true;
                   t += "  [" + std::to_string(i) + "] " + (l.region >= 0 && l.region < ai::REG_COUNT ? kRegions[l.region] : "?") +
                        (l.invert ? " (inverted)" : "") + " exposure " + fmt("%.2f", l.exposure) + " contrast " +
                        fmt("%.0f", l.contrast) + " temp " + fmt("%.0f", l.temp) + " saturation " + fmt("%.0f", l.saturation) + "\n";
                 }
                 if (!any) t += "  (none)\n";
                 t += "Photo layers:\n";
                 any = false;
                 for (int i = 0; i < kMaxImgLayers; i++) {
                   const ImageLayer& l = S.p.img[i];
                   if (!l.enabled) continue;
                   any = true;
                   t += "  [" + std::to_string(i) + "] " + l.path + " opacity " + fmt("%.0f", l.opacity) + " blend " +
                        kBlends[l.blend] + (l.keep ? " (AI cut-out)" : "") + (l.hidden ? " hidden" : "") + "\n";
                 }
                 if (!any) t += "  (none)\n";
                 t += "Magic-eraser fixes: " + std::to_string(S.p.heals) + "\nUndo steps available: " + std::to_string(S.undo.size());
                 Result r;
                 r.text = t;
                 return r;
               }});

  T.push_back({"list_adjustments", "What can be adjusted",
               "Lists every adjustment name with its range, for set_adjustments. Call once if unsure of a name.",
               [] { return schemaObj(); },
               [](Session&, const Json&) {
                 std::string t = "Global adjustments (set_adjustments \"values\"):\n";
                 for (const Adj& a : kAdj) t += "  " + std::string(a.key) + "  [" + fmt("%g", a.lo) + " .. " + fmt("%g", a.hi) + "]  " + a.what + "\n";
                 t += "Colour mixer per band (" ;
                 for (int i = 0; i < kHslBands; i++) t += std::string(i ? ", " : "") + std::to_string(i) + "=" + kBands[i];
                 t += "):\n  hslHue<band>, hslSat<band>, hslLum<band>  [-100 .. 100]  e.g. hslSat5 = blue saturation\n";
                 t += "Colour grading wheels (0=shadows 1=midtones 2=highlights 3=global):\n"
                      "  grade<n>Hue [0 .. 360], grade<n>Sat [0 .. 100], grade<n>Lum [-100 .. 100]\n";
                 t += "Tone curve: pass \"curve\": {\"channel\": \"rgb|r|g|b\", \"points\": [[0,0],[0.25,0.2],[0.75,0.8],[1,1]]}\n";
                 t += "Geometry is done by crop_rotate; areas by adjust_region; looks by apply_look.";
                 Result r;
                 r.text = t;
                 return r;
               }});

  T.push_back({"set_adjustments", "Adjust light and colour",
               "Sets any global adjustments at once, e.g. {\"values\": {\"exposure\": 0.4, \"contrast\": 15, \"temp\": 10}}. "
               "Values are absolute (not added). Unlisted values stay as they are. Returns the new look. "
               "See list_adjustments for every name and range.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "values", "object", "Map of adjustment name to number");
                 prop(s, "curve", "object", "Optional tone curve: {channel: rgb|r|g|b, points: [[x,y],...] 0..1}");
                 prop(s, "reset_first", "boolean", "Start from no tone/colour edits (crop and layers kept)");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 S.checkpoint();
                 if (a.flag("reset_first", false)) {
                   EditParams np;
                   copyGeometry(np, S.p);
                   copyLayers(np, S.p);
                   S.p = np;
                 }
                 std::string text, unknown;
                 if (const Json* v = a.get("values")) {
                   for (const auto& kv : v->o) {
                     if (!kv.second.isNum()) continue;
                     bool known = false;
                     for (const Adj& d : kAdj) known |= kv.first == d.key;
                     known |= kv.first.compare(0, 6, "hslHue") == 0 || kv.first.compare(0, 6, "hslSat") == 0 ||
                              kv.first.compare(0, 6, "hslLum") == 0 || kv.first.compare(0, 5, "grade") == 0;
                     if (!known) { unknown += " " + kv.first; continue; }
                     text += kv.first + "=" + fmt("%.6g", kv.second.n) + "\n";
                   }
                 }
                 if (const Json* c = a.get("curve")) {
                   std::string ch = c->str("channel", "rgb");
                   int idx = ch == "r" ? CURVE_R : ch == "g" ? CURVE_G : ch == "b" ? CURVE_B : CURVE_MASTER;
                   std::string pts;
                   if (const Json* ps = c->get("points"))
                     for (const Json& pt : ps->a)
                       if (pt.isArr() && pt.a.size() == 2 && pt.a[0].isNum() && pt.a[1].isNum())
                         pts += (pts.empty() ? "" : ";") + fmt("%.5g", pt.a[0].n) + "," + fmt("%.5g", pt.a[1].n);
                   if (!pts.empty()) text += "curve" + std::to_string(idx) + "=" + pts + "\n";
                 }
                 deserializeParams(text, S.p, true);
                 S.save();
                 return preview(S, unknown.empty() ? "Applied." : "Applied. Unknown names ignored:" + unknown);
               }});

  T.push_back({"auto_edit", "AI auto edit",
               "Lets the AI edit the photo for its scene (portrait, landscape, food, night...), including region "
               "adjustments for sky/people and rebuilding blown highlights. A good first step. strength 0-150 (default 100).",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "strength", "number", "0-150, default 100");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 if (!initAi(S, err)) return fail("GPU error: " + err);
                 if (!S.autoModels.load(err)) return fail("AI unavailable: " + err);
                 S.segment(err);
                 float emb[ai::kEmbDim];
                 bool hasEmb = aiCachedEmbedding(S.path, emb);
                 ai::AutoResult res;
                 if (!aiAutoEditFile(S.aiR, S.autoModels, S.path, hasEmb ? emb : nullptr, S.segOk ? &S.seg : nullptr,
                                     S.segOk ? &S.guide : nullptr, S.gw, S.gh, S.segOk, S.p, res, err, true))
                   return fail("Auto edit failed: " + err);
                 S.autoModels.release();
                 ai::ortUnload();
                 S.checkpoint();
                 float k = (float)std::clamp(a.num("strength", 100), 0.0, 150.0) / 100.f;
                 EditParams np = ai::blendStyle(S.p, res.params, k);
                 copyLayers(np, res.params);
                 std::memcpy(np.img, S.p.img, sizeof(np.img));
                 np.heals = S.p.heals;
                 S.p = np;
                 S.masksDirty = true;
                 S.recLoaded = false;
                 S.save();
                 return preview(S, "Auto edited (scene " + std::string(sceneName(res.scene)) + ", " + fmt("%.0f", res.sceneConf * 100) +
                                       "% sure). Fine-tune with set_adjustments if needed.",
                                1024, true);
               }});

  T.push_back({"apply_look", "Apply a look (preset)",
               "Applies a ready-made look. Built-in: Vivid, Matte Film, B&W Contrast, Warm Sunset, Cool Moody, Soft "
               "Portrait, plus the user's saved looks. Crop, regions and layers are kept. Call with no name to list them.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "name", "string", "Look name (English or Thai)");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::vector<Preset> ps = builtinPresets();
                 for (const std::string& f : listFiles(exeDir() + "\\presets", kLookExt)) {
                   Preset pr;
                   pr.nameEn = pr.nameTh = fileStem(f);
                   pr.builtin = false;
                   if (readTextFile(f, pr.data)) ps.push_back(pr);
                 }
                 std::string name = a.str("name");
                 if (name.empty()) {
                   Result r;
                   r.text = "Looks:";
                   for (const Preset& pr : ps) r.text += "\n  " + pr.nameEn + (pr.nameTh != pr.nameEn ? " / " + pr.nameTh : "");
                   return r;
                 }
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 for (const Preset& pr : ps) {
                   std::string en = pr.nameEn, n2 = name;
                   for (char& c : en) c = (char)tolower((unsigned char)c);
                   for (char& c : n2) c = (char)tolower((unsigned char)c);
                   if (en == n2 || pr.nameTh == name) {
                     S.checkpoint();
                     applyPreset(pr, S.p);
                     S.save();
                     return preview(S, "Applied the look " + pr.nameEn + ".", 1024, true);
                   }
                 }
                 return fail("No look named " + name + ". Call apply_look without a name to list them.");
               }});

  T.push_back({"crop_rotate", "Crop, straighten, rotate, flip",
               "Crop with x, y, w, h as fractions (0..1) of the whole uncropped frame, or aspect like \"1:1\", \"4:5\", "
               "\"16:9\" for the largest centred crop. straighten in degrees (-45..45), rotate90 adds quarter turns "
               "clockwise, flip_h / flip_v mirror. reset=true restores the full frame.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "x", "number", "Crop left (0..1)");
                 prop(s, "y", "number", "Crop top (0..1)");
                 prop(s, "w", "number", "Crop width (0..1)");
                 prop(s, "h", "number", "Crop height (0..1)");
                 prop(s, "aspect", "string", "e.g. 1:1, 4:5, 3:2, 16:9, 9:16");
                 prop(s, "straighten", "number", "Degrees, -45..45");
                 prop(s, "rotate90", "number", "Quarter turns clockwise to add (1, 2 or 3)");
                 prop(s, "flip_h", "boolean", "Mirror left-right");
                 prop(s, "flip_v", "boolean", "Mirror top-bottom");
                 prop(s, "reset", "boolean", "Undo all geometry");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 S.checkpoint();
                 EditParams& p = S.p;
                 if (a.flag("reset", false)) resetGeometry(p);
                 if (a.has("rotate90")) p.rotate90 = (p.rotate90 + (int)a.num("rotate90", 0)) & 3;
                 if (a.has("flip_h")) p.flipH = a.flag("flip_h", false) ? !p.flipH : p.flipH;
                 if (a.has("flip_v")) p.flipV = a.flag("flip_v", false) ? !p.flipV : p.flipV;
                 if (a.has("straighten")) p.straighten = (float)std::clamp(a.num("straighten", 0), -45.0, 45.0);
                 int fw, fh;
                 frameDims(p, S.R.srcW(), S.R.srcH(), S.R.orientation(), fw, fh);
                 std::string asp = a.str("aspect");
                 if (!asp.empty()) {
                   float n = 0, d = 0;
                   if (sscanf(asp.c_str(), "%f:%f", &n, &d) == 2 && n > 0 && d > 0) {
                     float want = n / d, have = (float)fw / fh;
                     p.cropW = want < have ? want / have : 1.f;
                     p.cropH = want < have ? 1.f : have / want;
                     p.cropX = (1 - p.cropW) / 2;
                     p.cropY = (1 - p.cropH) / 2;
                   }
                 }
                 if (a.has("w") || a.has("h") || a.has("x") || a.has("y")) {
                   p.cropX = (float)a.num("x", p.cropX);
                   p.cropY = (float)a.num("y", p.cropY);
                   p.cropW = (float)a.num("w", p.cropW);
                   p.cropH = (float)a.num("h", p.cropH);
                 }
                 constrainCrop(p, fw, fh);
                 sanitizeParams(p);
                 S.save();
                 return preview(S, "Geometry set.");
               }});

  T.push_back({"find_regions", "Find regions and faces (AI)",
               "Finds what is in the photo (people, faces, sky, water, trees, buildings, road...) with the share of "
               "the image each covers. Use the names with adjust_region. Takes ~2-3 s the first time.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 if (!S.segment(err)) return fail("Region AI unavailable: " + err);
                 std::vector<int> o;
                 for (int i = 0; i < ai::REG_COUNT; i++) o.push_back(i);
                 std::sort(o.begin(), o.end(), [&](int x, int y) { return S.seg.area[x] > S.seg.area[y]; });
                 std::string t = "Regions found (share of the photo):\n";
                 if (!S.seg.faces.empty()) t += "  face: " + std::to_string(S.seg.faces.size()) + " face(s)\n";
                 for (int r : o) {
                   if (r == ai::REG_FACE || S.seg.area[r] < 0.005f) continue;
                   t += "  " + std::string(kRegions[r]) + ": " + fmt("%.0f", S.seg.area[r] * 100) + "%\n";
                 }
                 t += "Also: \"background\" = everything except people.";
                 Result r;
                 r.text = t;
                 return r;
               }});

  T.push_back({"adjust_region", "Adjust one region only (AI mask)",
               "Edits only one area found by AI: face, person, sky, water, trees, grass, mountain, building, road, "
               "ground, vehicle, animal, walls, food, or background (= not people). invert=true edits everything "
               "except the region. Values: exposure (EV -3..3), contrast, highlights, shadows, temp, tint, "
               "saturation, clarity (-100..100), feather 0..100. Calling again for the same region updates it; "
               "remove=true deletes it. Up to 4 regions.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "region", "string", "Region name");
                 prop(s, "invert", "boolean", "Edit everything except this region");
                 for (const char* k : {"exposure", "contrast", "highlights", "shadows", "temp", "tint", "saturation", "clarity", "feather"})
                   prop(s, k, "number", "See the tool description for ranges");
                 prop(s, "remove", "boolean", "Delete this region adjustment");
                 required(s, {"region"});
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 std::string name = a.str("region");
                 bool inv = a.flag("invert", false);
                 if (name == "background") name = "person", inv = !inv;
                 int reg = regionIndex(name);
                 if (reg < 0) return fail("Unknown region " + a.str("region") + ". Call find_regions for what is in this photo.");
                 if (!S.segment(err)) return fail("Region AI unavailable: " + err);
                 S.checkpoint();
                 int slot = -1;
                 for (int i = 0; i < kMaxLayers && slot < 0; i++)
                   if (S.p.layers[i].enabled && S.p.layers[i].region == reg && (S.p.layers[i].invert != 0) == inv) slot = i;
                 if (a.flag("remove", false)) {
                   if (slot < 0) return fail("No adjustment on that region.");
                   std::memset((void*)&S.p.layers[slot], 0, sizeof(LocalLayer));
                 } else {
                   if (slot < 0) {
                     for (int i = 0; i < kMaxLayers && slot < 0; i++)
                       if (!S.p.layers[i].enabled) slot = i;
                     if (slot < 0) return fail("All 4 region slots are used; remove one first (adjust_region remove=true).");
                     std::memset((void*)&S.p.layers[slot], 0, sizeof(LocalLayer));
                     S.p.layers[slot].enabled = 1;
                     S.p.layers[slot].region = reg;
                     S.p.layers[slot].invert = inv;
                     S.p.layers[slot].feather = 10;
                   }
                   LocalLayer& l = S.p.layers[slot];
                   l.exposure = (float)a.num("exposure", l.exposure);
                   l.contrast = (float)a.num("contrast", l.contrast);
                   l.highlights = (float)a.num("highlights", l.highlights);
                   l.shadows = (float)a.num("shadows", l.shadows);
                   l.temp = (float)a.num("temp", l.temp);
                   l.tint = (float)a.num("tint", l.tint);
                   l.saturation = (float)a.num("saturation", l.saturation);
                   l.clarity = (float)a.num("clarity", l.clarity);
                   l.feather = (float)a.num("feather", l.feather);
                 }
                 sanitizeParams(S.p);
                 S.masksDirty = true;
                 S.save();
                 return preview(S, "Region adjustment saved.", 1024, true);
               }});

  T.push_back({"recover_highlights", "Rebuild blown highlights (AI)",
               "Rebuilds areas that are pure white (blown sky, faces) with plausible colour and texture. strength 0-100 "
               "(0 turns it off). Takes a few seconds the first time.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "strength", "number", "0-100, default 100");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 S.checkpoint();
                 S.p.recover = (float)std::clamp(a.num("strength", 100), 0.0, 100.0);
                 S.recLoaded = false;
                 S.R.clearRecover();
                 S.save();
                 return preview(S, S.p.recover > 0 ? "Highlight rebuild on (if the photo has blown areas)." : "Highlight rebuild off.", 1024, true);
               }});

  T.push_back({"magic_erase", "Magic eraser (AI removes objects)",
               "Removes unwanted things (people, wires, litter, spots) and fills in matching background. Give one or "
               "more strokes painted over the object: points are [x, y] in 0..1 of the current view image (x right, "
               "y down), radius is a fraction of the view width (e.g. 0.03). Cover the whole object plus its shadow. "
               "Each stroke takes a few seconds. Look at the result and add strokes for leftovers.",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "strokes", "array", "[{\"points\": [[x,y],...], \"radius\": 0.03}, ...]");
                 required(s, {"strokes"});
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 const Json* st = a.get("strokes");
                 if (!st || !st->isArr() || st->a.empty()) return fail("Give strokes: [{\"points\": [[x,y],...], \"radius\": 0.03}]");
                 Geometry g = S.geom();
                 float scale = (float)g.outW / std::max(1, S.R.orientedW());
                 S.checkpoint();
                 int added = 0;
                 for (const Json& s : st->a) {
                   HealStroke h;
                   h.radius = (float)s.num("radius", 0.03) * scale;
                   if (const Json* ps = s.get("points"))
                     for (const Json& pt : ps->a)
                       if (pt.isArr() && pt.a.size() == 2 && pt.a[0].isNum() && pt.a[1].isNum()) {
                         float ox, oy;
                         viewToOriented(g, (float)pt.a[0].n, (float)pt.a[1].n, ox, oy);
                         h.pts.push_back(ox);
                         h.pts.push_back(oy);
                       }
                   int before = S.p.heals;
                   S.heal.add(S.p, S.R, std::move(h));
                   added += S.p.heals > before;
                 }
                 if (!added) return fail("No usable stroke (points must be inside 0..1, radius > 0).");
                 S.heal.sync(S.p, S.R, S.path, true);
                 std::string e2 = S.heal.takeError();
                 S.save();
                 return preview(S, e2.empty() ? "Erased " + std::to_string(added) + " stroke(s)." : "Magic eraser failed: " + e2, 1024, true);
               }});

  auto layerSchema = [](bool add) {
    Json s = schemaObj();
    prop(s, "path", "string", "Base photo (optional)");
    if (add) prop(s, "image", "string", "Absolute path of the photo to place on top");
    else prop(s, "index", "number", "Layer index from get_info");
    prop(s, "x", "number", "Centre x in the view, 0..1");
    prop(s, "y", "number", "Centre y in the view, 0..1");
    prop(s, "width", "number", "Width as a fraction of the view width (e.g. 0.4)");
    prop(s, "rotate", "number", "Degrees clockwise");
    prop(s, "opacity", "number", "0-100");
    prop(s, "blend", "string", "normal, multiply, screen, overlay, soft-light, darken, lighten");
    prop(s, "cutout", "string", "AI background removal: none, subject (main subject), or region names joined by + (e.g. person+animal)");
    prop(s, "feather", "number", "Cut-out edge softness 0-100");
    prop(s, "match_colors", "boolean", "Match the layer's light and colour to the base photo");
    prop(s, "flip", "boolean", "Mirror the layer");
    prop(s, "hidden", "boolean", "Hide / show the layer");
    if (add) required(s, {"image"});
    else required(s, {"index"});
    return s;
  };
  auto applyLayer = [](Session& S, ImageLayer& l, const Json& a, std::string& note) {
    Geometry g = S.geom();
    float vx = (float)a.num("x", -1), vy = (float)a.num("y", -1);
    if (vx >= 0 && vy >= 0) {
      float ox, oy;
      viewToOriented(g, vx, vy, ox, oy);
      l.cx = ox;
      l.cy = oy;
    }
    if (a.has("width")) l.scale = (float)a.num("width", .5) * g.outW / std::max(1, S.R.orientedW());
    l.rotate = (float)a.num("rotate", l.rotate);
    l.opacity = (float)a.num("opacity", l.opacity);
    l.feather = (float)a.num("feather", l.feather);
    if (a.has("flip")) l.flipH = a.flag("flip", false);
    if (a.has("hidden")) l.hidden = a.flag("hidden", false);
    std::string bl = a.str("blend");
    for (int i = 0; i < BLEND_COUNT; i++)
      if (bl == kBlends[i]) l.blend = i;
    std::string cut = a.str("cutout");
    if (!cut.empty()) {
      S.layers.sync(S.p, S.R, true);  // the layer photo and its regions must be loaded
      LayerAsset* A = S.layers.asset(l.id);
      if (cut == "none") {
        l.keep = 0;
      } else if (!A || !A->segOk) {
        note += " AI cut-out unavailable for this layer.";
      } else if (cut == "subject") {
        const ai::SegResult& sg = A->seg;
        int keep = sg.area[ai::REG_PERSON] >= 0.02f ? 1 << ai::REG_PERSON : 0;
        int best = -1;
        for (int r : {ai::REG_ANIMAL, ai::REG_VEHICLE, ai::REG_FOOD})
          if (!keep && sg.area[r] >= 0.02f && (best < 0 || sg.area[r] > sg.area[best])) best = r;
        if (!keep && best >= 0) keep = 1 << best;
        l.keep = keep ? keep : 1 << ai::REG_PERSON;
      } else {
        int keep = 0;
        std::stringstream ss(cut);
        std::string part;
        while (std::getline(ss, part, '+')) {
          int r = regionIndex(part);
          if (r >= 0 && r != ai::REG_FACE) keep |= 1 << r;
          else note += " Unknown cut-out region " + part + ".";
        }
        if (keep) l.keep = keep;
      }
    }
  };

  T.push_back({"layer_add", "Place another photo on top (layer)",
               "Composites another photo onto this one, Photoshop-style. Position and size are in view coordinates. "
               "cutout=\"subject\" lets AI remove the layer's background (keeps the person/animal/vehicle). "
               "match_colors=true blends its light and colour into the scene. Up to 4 layers.",
               [layerSchema] { return layerSchema(true); },
               [applyLayer](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 std::string img = a.str("image"), why;
                 if (!layerPathAllowed(img, why)) return fail("Cannot use " + img + ": " + why);
                 int slot = -1;
                 for (int i = 0; i < kMaxImgLayers && slot < 0; i++)
                   if (!S.p.img[i].enabled) slot = i;
                 if (slot < 0) return fail("All 4 layer slots are used.");
                 S.checkpoint();
                 ImageLayer& l = S.p.img[slot];
                 std::memset((void*)&l, 0, sizeof(l));
                 l.enabled = 1;
                 l.id = S.layers.nextId(S.p);
                 l.cx = l.cy = .5f;
                 l.scale = .5f;
                 l.opacity = 100;
                 l.feather = 5;
                 std::memcpy(l.path, img.data(), std::min(img.size(), sizeof(l.path) - 1));
                 std::string note;
                 applyLayer(S, l, a, note);
                 if (a.flag("match_colors", false)) {
                   S.layers.sync(S.p, S.R, true);
                   S.layers.sync(S.p, S.R, false);
                   S.layers.matchColours(S.path, l);
                 }
                 sanitizeParams(S.p);
                 S.save();
                 return preview(S, "Layer " + std::to_string(slot) + " added." + note);
               }});

  T.push_back({"layer_update", "Change a layer",
               "Moves, resizes, rotates or restyles a photo layer (index from get_info).",
               [layerSchema] { return layerSchema(false); },
               [applyLayer](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 int i = (int)a.num("index", -1);
                 if (i < 0 || i >= kMaxImgLayers || !S.p.img[i].enabled) return fail("No layer with that index (see get_info).");
                 S.checkpoint();
                 std::string note;
                 applyLayer(S, S.p.img[i], a, note);
                 if (a.flag("match_colors", false)) S.layers.matchColours(S.path, S.p.img[i]);
                 sanitizeParams(S.p);
                 S.save();
                 return preview(S, "Layer " + std::to_string(i) + " updated." + note);
               }});

  T.push_back({"layer_remove", "Remove a layer",
               "Removes a photo layer (index from get_info).",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "index", "number", "Layer index");
                 required(s, {"index"});
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 int i = (int)a.num("index", -1);
                 if (i < 0 || i >= kMaxImgLayers || !S.p.img[i].enabled) return fail("No layer with that index.");
                 S.checkpoint();
                 for (int j = i; j + 1 < kMaxImgLayers; j++) S.p.img[j] = S.p.img[j + 1];
                 std::memset((void*)&S.p.img[kMaxImgLayers - 1], 0, sizeof(ImageLayer));
                 S.save();
                 return preview(S, "Layer removed.");
               }});

  T.push_back({"undo", "Undo / reset",
               "undo steps back one change (this session). reset=true removes all edits (keep_crop=true keeps the crop).",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "reset", "boolean", "Remove all edits instead of one step");
                 prop(s, "keep_crop", "boolean", "With reset: keep crop/rotation (default true)");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 if (a.flag("reset", false)) {
                   S.checkpoint();
                   EditParams np;
                   if (a.flag("keep_crop", true)) copyGeometry(np, S.p);
                   S.p = np;
                 } else {
                   if (S.undo.empty()) return fail("Nothing to undo in this session.");
                   S.p = S.undo.back();
                   S.undo.pop_back();
                 }
                 S.masksDirty = true;
                 S.recLoaded = false;
                 S.R.clearRecover();
                 S.save();
                 return preview(S, a.flag("reset", false) ? "All edits removed." : "Undone.");
               }});

  T.push_back({"export", "Save the edited photo",
               "Writes the edited photo at full resolution as JPEG or PNG. The original is never changed and an "
               "existing file is never overwritten (a number is added). long_edge resizes (0 = full size).",
               [] {
                 Json s = schemaObj();
                 prop(s, "path", "string", "Photo (optional)");
                 prop(s, "output", "string", "Output file path (.jpg or .png); default: <photo>_edit.jpg next to it");
                 prop(s, "quality", "number", "JPEG quality 1-100 (default 92)");
                 prop(s, "long_edge", "number", "Resize so the long edge is this many pixels (0 = full size)");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::string err;
                 if (!ensurePhoto(S, a, err)) return fail(err);
                 S.prepare();
                 Geometry g = S.geom();
                 int w = g.outW, h = g.outH, L = (int)a.num("long_edge", 0);
                 if (L > 0 && L < std::max(w, h)) {
                   double k = (double)L / std::max(w, h);
                   w = std::max(1, (int)std::lround(w * k));
                   h = std::max(1, (int)std::lround(h * k));
                 }
                 std::string out = a.str("output");
                 if (out.empty()) out = dirOf(S.path) + "\\" + fileStem(S.path) + "_edit.jpg";
                 bool png = out.size() > 4 && (out.substr(out.size() - 4) == ".png" || out.substr(out.size() - 4) == ".PNG");
                 std::string ext = png ? ".png" : ".jpg";
                 std::string stem = fileName(out);
                 stem = stem.substr(0, stem.find_last_of('.') == std::string::npos ? stem.size() : stem.find_last_of('.'));
                 out = freePath(dirOf(out), stem, ext);
                 std::vector<uint8_t> rgb((size_t)w * h * 3);
                 S.R.exportBegin(S.p, w, h);
                 while (!S.R.exportStep(rgb.data())) {}
                 if (!writeImage(out, rgb.data(), w, h, png, (int)std::clamp(a.num("quality", 92), 1.0, 100.0), err))
                   return fail("Write failed: " + err);
                 Result r;
                 r.text = "Saved " + out + " (" + std::to_string(w) + "x" + std::to_string(h) + ").";
                 return r;
               }});

  // ---- folders
  auto analyse = [](const std::string& folder, std::vector<ai::PhotoInfo>& photos, std::string& err) {
    if (!isDirectory(folder)) {
      err = "Not a folder: " + folder;
      return false;
    }
    ai::Analyzer an;
    an.startFolder(folder, true);
    an.wait();
    an.poll();
    photos = an.takeResults();
    if (photos.empty()) err = "No photos found in " + folder;
    return !photos.empty();
  };
  auto sheet = [](const std::string& folder, const std::vector<const ai::PhotoInfo*>& list) {
    const int cols = 6, cell = 200, n = (int)list.size(), rows = (n + cols - 1) / cols;
    int W = cols * cell, H = std::max(1, rows) * cell;
    std::vector<uint8_t> img((size_t)W * H * 3, 24);
    for (int i = 0; i < n; i++) {
      std::vector<uint8_t> jpg, rgb;
      int tw, th;
      if (!ai::readThumb(folder, list[i]->thumbOff, list[i]->thumbLen, jpg) || !decodeRgbMem(jpg.data(), jpg.size(), rgb, tw, th)) continue;
      ai::SmallImage si, fit;
      si.w = tw;
      si.h = th;
      si.rgb.swap(rgb);
      ai::fitLongEdge(si, cell - 8, fit);
      int ox = (i % cols) * cell + (cell - fit.w) / 2, oy = (i / cols) * cell + (cell - fit.h) / 2;
      for (int y = 0; y < fit.h; y++) std::memcpy(&img[((size_t)(oy + y) * W + ox) * 3], &fit.rgb[(size_t)y * fit.w * 3], (size_t)fit.w * 3);
    }
    return jpeg(img, W, H, 80);
  };

  T.push_back({"cull_folder", "Pick the best photos in a folder (AI)",
               "Scores every photo in a folder for sharpness, light and composition, groups burst shots and returns "
               "the best first, with reasons. Also returns a contact sheet of the top photos (row by row, same order "
               "as the list). The first run on a big folder can take a minute; results are cached.",
               [] {
                 Json s = schemaObj();
                 prop(s, "folder", "string", "Absolute folder path");
                 prop(s, "top", "number", "How many to return (default 12, max 60)");
                 prop(s, "best_of_bursts", "boolean", "Keep only the best of near-duplicates (default true)");
                 required(s, {"folder"});
                 return s;
               },
               [analyse, sheet](Session&, const Json& a) {
                 std::string folder = a.str("folder"), err;
                 std::vector<ai::PhotoInfo> photos;
                 if (!analyse(folder, photos, err)) return fail(err);
                 ai::RankSettings rs;
                 rs.dedupe = a.flag("best_of_bursts", true);
                 auto groups = ai::groupDuplicates(photos);
                 auto ranked = ai::rankPhotos(photos, groups, {}, rs);
                 int top = (int)std::clamp(a.num("top", 12), 1.0, 60.0);
                 Result r;
                 r.text = std::to_string(photos.size()) + " photos analysed. Best first:\n";
                 std::vector<const ai::PhotoInfo*> list;
                 for (int i = 0; i < (int)ranked.size() && i < top; i++) {
                   const auto& e = ranked[i];
                   const ai::PhotoInfo& p = photos[e.idx];
                   list.push_back(&p);
                   std::string why;
                   for (int k = 0; k < ai::R_COUNT_BITS; k++)
                     if (e.reasons & (1u << k)) why += std::string(why.empty() ? "" : ", ") + kReasons[k];
                   r.text += std::to_string(i + 1) + ". " + p.path + "  score " + fmt("%.0f", e.score) +
                             (e.groupSize > 1 ? "  (best of " + std::to_string(e.groupSize) + ")" : "") +
                             (why.empty() ? "" : "  [" + why + "]") + "\n";
                 }
                 r.images.push_back(sheet(folder, list));
                 return r;
               }});

  T.push_back({"search_photos", "Find photos by description (AI)",
               "Searches a folder by meaning, in English or Thai, e.g. \"kite on the beach\", \"คนใส่แว่น\". Returns "
               "the closest matches and a contact sheet.",
               [] {
                 Json s = schemaObj();
                 prop(s, "folder", "string", "Absolute folder path");
                 prop(s, "query", "string", "What to look for");
                 prop(s, "top", "number", "How many (default 12)");
                 required(s, {"folder", "query"});
                 return s;
               },
               [analyse, sheet](Session&, const Json& a) {
                 std::string folder = a.str("folder"), err;
                 std::vector<ai::PhotoInfo> photos;
                 if (!analyse(folder, photos, err)) return fail(err);
                 ai::TextQuery q;
                 float emb[ai::kEmbDim];
                 std::string english;
                 if (!q.load(err) || !q.embed(a.str("query"), emb, &english, err)) return fail("Search AI unavailable: " + err);
                 q.release();
                 ai::ortUnload();
                 std::vector<std::pair<float, int>> s;
                 for (int i = 0; i < (int)photos.size(); i++) {
                   if (!photos[i].hasAi) continue;
                   float d = 0;
                   for (int k = 0; k < ai::kEmbDim; k++) d += photos[i].emb[k] * emb[k];
                   s.push_back({d, i});
                 }
                 std::sort(s.begin(), s.end(), [](auto& x, auto& y) { return x.first > y.first; });
                 int top = (int)std::clamp(a.num("top", 12), 1.0, 60.0);
                 Result r;
                 r.text = "Searched for \"" + english + "\". Closest first:\n";
                 std::vector<const ai::PhotoInfo*> list;
                 for (int i = 0; i < (int)s.size() && i < top; i++) {
                   list.push_back(&photos[s[i].second]);
                   r.text += std::to_string(i + 1) + ". " + photos[s[i].second].path + "  match " + fmt("%.3f", s[i].first) + "\n";
                 }
                 r.images.push_back(sheet(folder, list));
                 return r;
               }});

  T.push_back({"batch_auto_edit", "Auto edit many photos",
               "AI-edits many photos at once (each for its own scene), saving the edits beside each photo; optionally "
               "exports full-size JPEGs to export_dir. Give a folder or a list of paths. skip_edited (default true) "
               "leaves photos the user already edited alone. About 1-3 s per photo.",
               [] {
                 Json s = schemaObj();
                 prop(s, "folder", "string", "All photos in this folder");
                 prop(s, "paths", "array", "Or a list of photo paths");
                 prop(s, "strength", "number", "0-150, default 100");
                 prop(s, "skip_edited", "boolean", "Skip photos that already have edits (default true)");
                 prop(s, "export_dir", "string", "Also save JPEGs here (optional)");
                 return s;
               },
               [](Session& S, const Json& a) {
                 std::vector<std::string> paths;
                 if (const Json* ps = a.get("paths"))
                   for (const Json& x : ps->a)
                     if (x.isStr()) paths.push_back(x.s);
                 if (!a.str("folder").empty()) {
                   auto more = listImages(a.str("folder"));
                   paths.insert(paths.end(), more.begin(), more.end());
                 }
                 if (paths.empty()) return fail("Give a folder or paths.");
                 std::string err;
                 if (!initAi(S, err)) return fail("GPU error: " + err);
                 if (!S.autoModels.load(err)) return fail("AI unavailable: " + err);
                 float k = (float)std::clamp(a.num("strength", 100), 0.0, 150.0) / 100.f;
                 bool skip = a.flag("skip_edited", true);
                 std::string outDir = a.str("export_dir");
                 if (!outDir.empty()) ensureDir(outDir);
                 int done = 0, skipped = 0, failed = 0, exported = 0;
                 for (const std::string& path : paths) {
                   EditParams base;
                   std::string sc;
                   bool edited = readSidecar(path, sc);
                   if (edited) deserializeParams(sc, base, true), edited = !isDefault(base);
                   EditParams out = base;
                   if (edited && skip) {
                     skipped++;
                   } else {
                     float emb[ai::kEmbDim];
                     bool hasEmb = aiCachedEmbedding(path, emb);
                     ai::AutoResult res;
                     std::string e2;
                     if (!aiAutoEditFile(S.aiR, S.autoModels, path, hasEmb ? emb : nullptr, nullptr, nullptr, 0, 0, true, base, res, e2, true)) {
                       failed++;
                       continue;
                     }
                     out = ai::blendStyle(base, res.params, k);
                     copyLayers(out, res.params);
                     std::memcpy(out.img, base.img, sizeof(out.img));
                     out.heals = base.heals;
                     std::string keep;
                     std::stringstream ss(sc);
                     std::string line;
                     while (std::getline(ss, line))
                       if (line.compare(0, 6, "stroke") == 0 || line.compare(0, 5, "heal=") == 0) keep += line + "\n";
                     sc = serializeParams(out, true) + keep;
                     writeSidecar(path, sc);
                     done++;
                   }
                   if (!outDir.empty()) {
                     ImageData img;
                     std::vector<uint8_t> rgb;
                     int w, h;
                     std::string e2;
                     if (loadImage(path, S.aiR.maxTextureSize(), img, e2) && renderEdited(S.aiR, path, out, sc, img, rgb, w, h, e2) &&
                         writeImage(freePath(outDir, fileStem(path), ".jpg"), rgb.data(), w, h, false, 92, e2))
                       exported++;
                     else
                       failed++;
                   }
                 }
                 S.autoModels.release();
                 ai::ortUnload();
                 if (S.loaded()) {  // the open photo may have been one of them
                   std::string e3;
                   std::string cur = S.path;
                   S.open(cur, e3);
                 }
                 Result r;
                 r.text = "Auto edited " + std::to_string(done) + ", skipped " + std::to_string(skipped) + " already edited, failed " +
                          std::to_string(failed) + (outDir.empty() ? "" : ", exported " + std::to_string(exported) + " JPEGs to " + outDir) + ".";
                 return r;
               }});

  return T;
}

// ---------------------------------------------------------------- JSON-RPC over stdio
HANDLE g_in = INVALID_HANDLE_VALUE, g_out = INVALID_HANDLE_VALUE;

void send(const Json& msg) {
  std::string s = json::dump(msg) + "\n";
  DWORD wr = 0;
  const char* p = s.data();
  size_t left = s.size();
  while (left > 0 && WriteFile(g_out, p, (DWORD)std::min<size_t>(left, 1 << 20), &wr, nullptr) && wr > 0) p += wr, left -= wr;
}

bool readLine(std::string& buf, std::string& line) {
  for (;;) {
    size_t nl = buf.find('\n');
    if (nl != std::string::npos) {
      line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    char tmp[65536];
    DWORD rd = 0;
    if (!ReadFile(g_in, tmp, sizeof(tmp), &rd, nullptr) || rd == 0) return false;  // client closed
    buf.append(tmp, rd);
    if (buf.size() > (64u << 20)) return false;  // no sane message is this large
  }
}

Json reply(const Json& id, Json result) {
  Json m = Json::object();
  m.set("jsonrpc", "2.0");
  m.set("id", id);
  m.set("result", std::move(result));
  return m;
}
Json replyErr(const Json& id, int code, const std::string& msg) {
  Json m = Json::object();
  m.set("jsonrpc", "2.0");
  m.set("id", id);
  Json e = Json::object();
  e.set("code", code);
  e.set("message", msg);
  m.set("error", e);
  return m;
}

const char* kInstructions =
    "Circle Room edits photos non-destructively (edits are saved next to each photo as .pedit; originals are never "
    "changed) and shows you the result as an image after every editing step, so look at it before deciding the next "
    "one. Typical flow: open_photo -> auto_edit (or set_adjustments) -> view compare=true -> refine (set_adjustments, "
    "adjust_region for face/sky/person, crop_rotate, apply_look, magic_erase to remove objects, layer_add to "
    "composite) -> export. For a whole folder: cull_folder to pick the best, search_photos to find by description, "
    "batch_auto_edit to edit many at once. Coordinates are 0..1 of the current view image (after crop), x to the "
    "right, y down. undo steps back. The user can open the same photos in the Circle Room app to see your edits.";

}  // namespace

int mcpServe(std::string& err) {
  // Our protocol channel is the original stdout; anything else that prints goes to stderr.
  g_in = GetStdHandle(STD_INPUT_HANDLE);
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE || !g_in || !g_out) {
    err = "no stdin/stdout: run this from an MCP client";
    return 2;
  }
  // Own copy of the reply pipe: redirecting the CRT's stdout below closes the handle it held.
  HANDLE dup = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), g_out, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) g_out = dup;
  HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
  SetStdHandle(STD_OUTPUT_HANDLE, herr ? herr : INVALID_HANDLE_VALUE);
  if (_fileno(stdout) >= 0 && _fileno(stderr) >= 0) _dup2(_fileno(stderr), _fileno(stdout));

  Session S;
  if (!S.R.init(err)) return 4;
  std::vector<Tool> T = tools();
  AgentLog activity;  // lets the open Circle Room window show what we are doing
  int callNo = 0;
  std::string buf, line;
  while (readLine(buf, line)) {
    if (line.empty()) continue;
    Json msg;
    if (!json::parse(line, msg) || !msg.isObj()) {
      send(replyErr(Json(), -32700, "parse error"));
      continue;
    }
    std::string method = msg.str("method");
    const Json* idp = msg.get("id");
    if (!idp) continue;  // notifications (initialized, cancelled) need no answer
    Json id = *idp;
    const Json* params = msg.get("params");
    Json noParams = Json::object();
    if (!params) params = &noParams;
    if (method == "initialize") {
      std::string ver = params->str("protocolVersion", "2025-06-18");
      if (ver != "2024-11-05" && ver != "2025-03-26" && ver != "2025-06-18") ver = "2025-06-18";
      Json r = Json::object();
      r.set("protocolVersion", ver);
      Json caps = Json::object();
      caps.set("tools", Json::object());
      r.set("capabilities", caps);
      Json info = Json::object();
      info.set("name", "circle-room");
      info.set("title", "Circle Room photo editor");
      info.set("version", "1.3.0");
      r.set("serverInfo", info);
      r.set("instructions", kInstructions);
      send(reply(id, r));
      const Json* ci = params->get("clientInfo");
      activity.open(ci && ci->isObj() ? ci->str("name") : std::string());
    } else if (method == "ping") {
      send(reply(id, Json::object()));
    } else if (method == "tools/list") {
      Json list = Json::array();
      for (const Tool& t : T) {
        Json j = Json::object();
        j.set("name", t.name);
        j.set("title", t.title);
        j.set("description", t.desc);
        j.set("inputSchema", t.schema());
        list.push(j);
      }
      Json r = Json::object();
      r.set("tools", list);
      send(reply(id, r));
    } else if (method == "tools/call") {
      std::string name = params->str("name");
      const Json* args = params->get("arguments");
      Json noArgs = Json::object();
      if (!args || !args->isObj()) args = &noArgs;
      const Tool* tool = nullptr;
      for (const Tool& t : T)
        if (name == t.name) tool = &t;
      if (!tool) {
        send(replyErr(id, -32602, "unknown tool: " + name));
        continue;
      }
      int callId = ++callNo;
      EditParams before = S.p;
      std::string beforePath = S.path;
      std::string target = args->str("path");
      if (target.empty() || name == "cull_folder" || name == "search_photos" || name == "batch_auto_edit") target = S.path;
      activity.call(callId, name, target, argSummary(*args));
      Result res;
      try {
        res = tool->run(S, *args);
      } catch (const std::exception& e) {
        res = fail(std::string("internal error: ") + e.what());
      }
      {
        // A tool that opened another photo compares against that photo as it was loaded.
        const EditParams& from = S.path == beforePath ? before : (S.undo.empty() ? S.p : S.undo.front());
        std::vector<AgentChange> changes;
        if (S.loaded() && !res.error) changes = editChanges(from, S.p);
        std::string first = res.text.substr(0, res.text.find('\n'));
        activity.done(callId, name, !res.error, S.loaded() ? S.path : target, changes, clip(first, 220));
      }
      Json content = Json::array();
      Json t = Json::object();
      t.set("type", "text");
      t.set("text", res.text);
      content.push(t);
      for (const auto& im : res.images) {
        Json j = Json::object();
        j.set("type", "image");
        j.set("data", b64(im));
        j.set("mimeType", "image/jpeg");
        content.push(j);
      }
      Json r = Json::object();
      r.set("content", content);
      r.set("isError", res.error);
      send(reply(id, r));
    } else {
      send(replyErr(id, -32601, "method not found: " + method));
    }
  }
  activity.close();
  S.heal.clear(S.R);
  S.layers.clear(S.R);
  S.R.shutdown();
  if (S.aiInit) S.aiR.shutdown();
  return 0;
}

std::vector<McpClient> mcpClients() {
  std::string exe = exeDir() + "\\CircleRoom.exe";
  std::string jexe;
  json::detail::esc(jexe, exe);  // JSON-escaped, quotes included
  // The common "mcpServers" block most agents read (Claude Desktop, Cursor, Gemini, Qwen, Kimi, DeepSeek TUI, ...).
  std::string common = "{\n  \"mcpServers\": {\n    \"circle-room\": {\n      \"command\": " + jexe +
                       ",\n      \"args\": [\"--mcp\"]\n    }\n  }\n}";
  // OpenCode-style "mcp" block (OpenCode 1.x, MiMo Code): a local server with a command array.
  auto local = [&](const char* schema, bool nested) {
    std::string server = "\"circle-room\": {\n" + std::string(nested ? "        " : "      ") + "\"type\": \"local\",\n" +
                         (nested ? "        " : "      ") + "\"command\": [" + jexe + ", \"--mcp\"]" +
                         (nested ? "" : ",\n      \"enabled\": true") + "\n" + (nested ? "      }" : "    }");
    return "{\n  \"$schema\": \"" + std::string(schema) + "\",\n  \"mcp\": {\n    " +
           (nested ? "\"servers\": {\n      " + server + "\n    }" : server) + "\n  }\n}";
  };
  std::vector<McpClient> c;
  c.push_back({"Claude Code", T_McpClaudeHow, {"claude mcp add --scope user circle-room -- \"" + exe + "\" --mcp"}});
  c.push_back({"Codex (OpenAI)", T_McpCodexHow,
               {"[mcp_servers.circle-room]\ncommand = '" + exe + "'\nargs = [\"--mcp\"]\nstartup_timeout_sec = 30\ntool_timeout_sec = 600"}});
  c.push_back({"OpenCode", T_McpOpenCodeHow,
               {local("https://opencode.ai/config.json", false), local("https://opencode.ai/config.json", true)}});
  c.push_back({"MiMo Code (Xiaomi)", T_McpMimoHow, {local("https://mimo.xiaomi.com/mimocode/config.json", false)}});
  c.push_back({"Kimi Code", T_McpKimiHow, {"kimi mcp add --transport stdio circle-room -- \"" + exe + "\" --mcp", common}});
  c.push_back({"Qwen Code", T_McpQwenHow, {common}});
  c.push_back({"DeepSeek TUI", T_McpDeepSeekHow, {common}});
  c.push_back({"Z Code (Zhipu)", T_McpZcodeHow, {"Command:   " + exe + "\nArguments: --mcp", common}});
  c.push_back({"Antigravity", T_McpAntigravityHow, {common}});
  c.push_back({"Cursor / Claude Desktop / Gemini CLI / Windsurf", T_McpJsonHow, {common}});
  c.push_back({"VS Code (Copilot)", T_McpVscodeHow,
               {"{\n  \"servers\": {\n    \"circle-room\": {\n      \"type\": \"stdio\",\n      \"command\": " + jexe +
                ",\n      \"args\": [\"--mcp\"]\n    }\n  }\n}"}});
  return c;
}
