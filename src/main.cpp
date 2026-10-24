// Circle Room: a lightweight, GPU-accelerated photo editor.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gl_loader.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "ai/cull.h"
#include "ai/segment.h"
#include "ai/highlight.h"
#include "ai/style_match.h"
#include "ai/text_query.h"
#include "history.h"
#include "i18n.h"
#include "image_io.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "heal.h"
#include "layers.h"
#include "params.h"
#include "platform_win.h"
#include "renderer.h"
#include "selftest.h"
#include "ui_cull.h"
#include "ui_icons.h"
#include "ui_panels.h"

extern "C" {
// Ask hybrid-GPU laptops to run us on the discrete GPU.
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
void* glfwGetWin32Window(GLFWwindow* window);
}

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- settings
struct Settings {
  int lang = 0;
  int winW = 1440, winH = 900, maximized = 1;
  std::string lastDir;
  int expPng = 0, expQuality = 92, expResize = 0, expLong = 2048;
  int tutorialDone = 0;
  int textPct = 100;  // text size, % of the system DPI scale
};

static std::string settingsPath() { return exeDir() + "\\settings.ini"; }
static std::string presetsDir() { return exeDir() + "\\presets"; }

static void loadSettings(Settings& s) {
  std::string t;
  if (!readTextFile(settingsPath(), t)) return;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t e = t.find('\n', pos);
    std::string line = t.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    pos = e == std::string::npos ? t.size() : e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    int iv = atoi(v.c_str());
    if (k == "lang") s.lang = iv;
    else if (k == "winW") s.winW = std::max(640, iv);
    else if (k == "winH") s.winH = std::max(480, iv);
    else if (k == "maximized") s.maximized = iv;
    else if (k == "lastDir") s.lastDir = v;
    else if (k == "expPng") s.expPng = iv;
    else if (k == "expQuality") s.expQuality = std::clamp(iv, 10, 100);
    else if (k == "expResize") s.expResize = iv;
    else if (k == "expLong") s.expLong = std::max(64, iv);
    else if (k == "tutorialDone") s.tutorialDone = iv;
    else if (k == "textPct") s.textPct = iv;
  }
}

static void saveSettings(const Settings& s) {
  char buf[256];
  snprintf(buf, sizeof(buf), "lang=%d\nwinW=%d\nwinH=%d\nmaximized=%d\nexpPng=%d\nexpQuality=%d\nexpResize=%d\nexpLong=%d\ntutorialDone=%d\ntextPct=%d\n",
           s.lang, s.winW, s.winH, s.maximized, s.expPng, s.expQuality, s.expResize, s.expLong, s.tutorialDone,
           s.textPct);
  writeTextFile(settingsPath(), std::string(buf) + "lastDir=" + s.lastDir + "\n");
}

// ---------------------------------------------------------------- app state
struct View {
  bool fit = true;
  float zoom = 1.f;
  float cx = 0.5f, cy = 0.5f;  // output coords at the canvas centre
};

struct RenderKey {  // everything the preview depends on; compared with memcmp
  EditParams p;
  float o[4];
  int w, h;
  float z;
  int crop, clip, before;
  int maskShow, maskVer;
};


struct LoadJob {
  std::thread th;
  std::atomic<bool> done{false};
  bool active = false, ok = false;
  std::string path, err;
  ImageData img;
};

struct ExportJob {
  int stage = 0;  // 0 idle, 1 rendering tiles on the GPU, 2 encoding on a worker thread
  std::vector<uint8_t> rgb;
  int w = 0, h = 0, quality = 92;
  bool png = false, ok = false;
  std::string path, err;
  std::thread th;
  std::atomic<bool> done{false};
};

struct App {
  GLFWwindow* win = nullptr;
  float scale = 1.f;     // dpiScale * text size; every UI size is multiplied by this
  float dpiScale = 1.f;
  int pendingTextPct = 0;  // text size to apply before the next frame
  Renderer R;
  Settings S;
  std::string gpuName;

  bool hasImage = false;
  std::string path;
  EditParams params;
  History hist;
  EditResult er;  // edits made during the current frame

  PanelState ps;
  View view;
  bool showBefore = false, split = false, splitDrag = false;
  float splitPos = 0.5f;
  bool cropMode = false, constrain = true;
  int aspectIdx = 0;
  int cropHandle = -1;
  EditParams cropStart;
  ImVec2 cropMouse;
  float cropUser[4] = {0, 0, 1, 1};  // last crop the user set; straighten re-fits from it
  bool clipWarn = false, showHisto = true;
  int page = 1;  // tool page shown next to the rail (Page)
  float viewZoom = 1.f;

  RenderKey lastKey;
  bool keyValid = false;
  unsigned texAfter = 0, texBefore = 0;
  float uA = 1, vA = 1, uB = 1, vB = 1;
  unsigned histo[4][256] = {};
  EditParams histParams;
  bool histValid = false;

  std::vector<Preset> presets;
  bool hoverActive = false;
  EditParams hoverParams;

  LoadJob load;
  ExportJob exp;
  std::string toast;
  Clock::time_point toastUntil;
  bool openExport = false, openSavePreset = false;
  char presetName[64] = {};
  std::vector<std::string> drops;

  // AI
  int mode = 0;  // 0 = edit, 1 = AI cull
  int tourStep = -1;  // tutorial: -1 = not running
  bool tourAutoTried = false;
  CullView cull;
  StyleLibrary styles;
  Renderer* aiR = nullptr;  // small renderer for style matching, created on first use
  EditParams styleOpt;
  bool styleValid = false;
  float styleStrength = 100.f;
  unsigned styleGroups = ai::SG_ALL;
  bool openNewStyle = false;
  char newStyleName[64] = {};

  // AI regions of the open photo (segmentation runs on a worker thread)
  struct SegJob {
    std::thread th;
    std::atomic<bool> done{false};
    bool active = false, ok = false;
    std::string path, err;
    ai::SegResult seg;
    std::vector<float> guide;
    int W = 0, H = 0;
  } segJob;
  ai::SegResult seg;
  std::vector<float> guide;
  int maskW = 0, maskH = 0, maskVer = 0;
  std::vector<uint8_t> maskRGBA;  // CPU copy so a single layer can be rebuilt
  std::string segPath;
  int chanRegion[kMaxLayers] = {-2, -2, -2, -2}, chanInvert[kMaxLayers] = {};
  float chanFeather[kMaxLayers] = {};
  int hoverLayer = -1;
  bool localPanelOpen = false;
  // AI highlight recovery (worker thread; overlay lives on the GPU)
  struct RecJob {
    std::thread th;
    std::atomic<bool> done{false};
    bool active = false, ok = false, segOk = false;
    std::string path, err;
    ai::SegResult seg;
    ai::Recovery rec;
  } recJob;
  std::string recPath;  // photo whose overlay is loaded
  // AI auto edit
  EditParams autoOpt;
  bool autoValid = false;
  float autoStrength = 100.f, autoConf = 0.f;
  int autoScene = -1;
  // Photo layers (compositing)
  LayerStore layers;
  int selLayer = -1;    // slot in params.img
  int layerTool = 0;    // 0 move / resize / rotate, 1 eraser, 2 restore brush
  float brushPx = 60.f, brushHard = 50.f;  // brush diameter on screen, hardness 0..100
  int layerDrag = 0;    // 0 none, 1 move, 2 resize, 3 rotate, 4 brush
  ImageLayer layerStart;
  float dragX = 0, dragY = 0;  // drag start, oriented pixels of the base photo
  // Magic eraser (LaMa fills painted strokes; patches lie under the layers)
  HealStore heal;
  HealStroke healPend;
  int editPage = 1;  // page the Edit tab returns to after Compose
};

static App* g_app = nullptr;

static void showToast(App& a, const std::string& msg) {
  a.toast = msg;
  a.toastUntil = Clock::now() + std::chrono::seconds(4);
}

static void loadPresets(App& a) {
  a.presets = builtinPresets();
  std::vector<std::string> files = listFiles(presetsDir(), kLookExt), old = listFiles(presetsDir(), kLegacyLookExt);
  files.insert(files.end(), old.begin(), old.end());
  for (const std::string& f : files) {
    Preset p;
    p.nameEn = fileStem(f);
    p.builtin = false;
    if (readTextFile(f, p.data)) a.presets.push_back(p);
  }
}

static void storeCropUser(App& a) {
  a.cropUser[0] = a.params.cropX;
  a.cropUser[1] = a.params.cropY;
  a.cropUser[2] = a.params.cropW;
  a.cropUser[3] = a.params.cropH;
}

static void frameOf(App& a, const EditParams& p, int& w, int& h) {
  frameDims(p, a.R.srcW(), a.R.srcH(), a.R.orientation(), w, h);
}

static void saveSidecar(App& a) {
  if (!a.hasImage) return;
  writeSidecar(a.path, isDefault(a.params) ? std::string()
                                           : serializeParams(a.params, true) + a.layers.serializeStrokes(a.params) +
                                                 a.heal.serialize(a.params));
}

static void paramsReplaced(App& a) {
  storeCropUser(a);
  saveSidecar(a);
}

static void openImage(App& a, const std::string& path) {
  if (a.load.active || path.empty()) return;
  a.load.active = true;
  a.load.done = false;
  a.load.path = path;
  int maxDim = a.R.maxTextureSize();
  a.load.th = std::thread([&L = a.load, maxDim]() {
    try {
      L.ok = loadImage(L.path, maxDim, L.img, L.err);
    } catch (const std::exception& e) {
      L.ok = false;
      L.err = e.what();
    }
    L.done = true;
    glfwPostEmptyEvent();
  });
}

static void clearSegmentation(App& a);
static void startSegmentation(App& a);
static void startRecover(App& a);
static void clearRecover(App& a);

static void finishLoad(App& a) {
  a.load.th.join();
  a.load.active = false;
  if (!a.load.ok) {
    showToast(a, std::string(tr(T_LoadFailed)) + ": " + fileName(a.load.path) + " (" + a.load.err + ")");
    return;
  }
  std::string err;
  if (!a.R.upload(a.load.img, err)) {
    a.load.img.release();
    showToast(a, std::string(tr(T_LoadFailed)) + ": " + err);
    return;
  }
  trimWorkingSet();
  a.hasImage = true;
  a.path = a.load.path;
  a.S.lastDir = dirOf(a.path);
  EditParams p;
  std::string sc;
  if (readSidecar(a.path, sc)) deserializeParams(sc, p, true);
  a.layers.clear(a.R);
  a.layers.parseStrokes(sc);
  a.heal.clear(a.R);
  a.heal.parse(sc);
  a.selLayer = -1;
  a.layerDrag = 0;
  a.params = p;
  a.hist.reset(p, T_Import);
  storeCropUser(a);
  a.view = View();
  a.cropMode = false;
  a.keyValid = false;
  a.histValid = false;
  clearSegmentation(a);
  clearRecover(a);
  if (a.params.recover > 0) {
    ai::Recovery rec;
    if (ai::loadRecoverCache(a.path, rec)) {
      a.R.setRecover(rec.rgba.data(), rec.w, rec.h);
      a.recPath = a.path;
    } else {
      startRecover(a);
    }
  }
  a.autoValid = false;
  if (anyLayer(a.params) || a.localPanelOpen) startSegmentation(a);
  std::string title = "Circle Room - " + fileName(a.path);
  glfwSetWindowTitle(a.win, title.c_str());
}

static void outputDims(App& a, int& w, int& h) {
  Geometry g = computeGeometry(a.params, a.R.srcW(), a.R.srcH(), a.R.orientation(), false);
  w = g.outW;
  h = g.outH;
  if (a.S.expResize) {
    int L = std::max(w, h);
    if (a.S.expLong < L) {
      double s = (double)a.S.expLong / L;
      w = std::max(1, (int)std::lround(w * s));
      h = std::max(1, (int)std::lround(h * s));
    }
  }
}

static void startExport(App& a, const std::string& out) {
  ExportJob& e = a.exp;
  outputDims(a, e.w, e.h);
  e.png = a.S.expPng != 0;
  e.quality = a.S.expQuality;
  e.path = out;
  e.rgb.assign((size_t)e.w * e.h * 3, 0);
  e.done = false;
  a.R.exportBegin(a.params, e.w, e.h);
  e.stage = 1;
}

static void pumpExport(App& a) {
  ExportJob& e = a.exp;
  if (e.stage == 1) {
    auto t0 = Clock::now();
    while (Clock::now() - t0 < std::chrono::milliseconds(30)) {
      if (a.R.exportStep(e.rgb.data())) {
        e.stage = 2;
        e.th = std::thread([&e]() {
          try {
            e.ok = writeImage(e.path, e.rgb.data(), e.w, e.h, e.png, e.quality, e.err);
          } catch (const std::exception& x) {
            e.ok = false;
            e.err = x.what();
          }
          std::vector<uint8_t>().swap(e.rgb);
          e.done = true;
          glfwPostEmptyEvent();
        });
        break;
      }
    }
  } else if (e.stage == 2 && e.done) {
    e.th.join();
    e.stage = 0;
    trimWorkingSet();
    showToast(a, e.ok ? std::string(tr(T_Saved)) + ": " + fileName(e.path) : std::string(tr(T_ExportFailed)) + ": " + e.err);
  }
}

static void doUndo(App& a) {
  if (!a.hist.canUndo()) return;
  a.params = a.hist.undo();
  paramsReplaced(a);
}
static void doRedo(App& a) {
  if (!a.hist.canRedo()) return;
  a.params = a.hist.redo();
  paramsReplaced(a);
}
static void doOpenDialog(App& a) {
  std::string f = openImageDialog(glfwGetWin32Window(a.win), a.S.lastDir);
  if (!f.empty()) openImage(a, f);
}

static Renderer* aiRenderer(App& a) {
  if (!a.aiR) {
    a.aiR = new Renderer();
    std::string err;
    if (!a.aiR->init(err)) {
      delete a.aiR;
      a.aiR = nullptr;
    }
  }
  return a.aiR;
}

static CullHost cullHost(App& a) {
  CullHost h;
  h.hwnd = glfwGetWin32Window(a.win);
  h.scale = a.scale;
  h.openInEditor = [&a](const std::string& path) {
    a.mode = 0;
    openImage(a, path);
  };
  h.toast = [&a](const std::string& m) { showToast(a, m); };
  h.aiRenderer = [&a]() { return aiRenderer(a); };
  return h;
}

// Embedding for the open photo: folder cache, else a one-off CLIP run (~1 s).
static bool currentEmbedding(App& a, float* emb) {
  if (aiCachedEmbedding(a.path, emb)) return true;
  ai::SmallImage si;
  std::string err;
  if (!ai::decodeScaled(a.path, 768, si, nullptr, err)) return false;
  ai::AnalyzeModels m;
  bool ok = m.clip.load(false, err);
  ai::PhotoInfo info;
  if (ok) ai::analyzeImage(si, &m, info, nullptr);
  m.clip.release();
  ai::ortUnload();
  if (ok && info.hasAi) std::memcpy(emb, info.emb, sizeof(info.emb));
  return ok && info.hasAi;
}

static void applyAiStyle(App& a) {
  const auto& sets = a.styles.sets();
  if (sets.empty() || !a.hasImage) return;
  Renderer* r = aiRenderer(a);
  if (!r) return;
  float emb[ai::kEmbDim];
  bool has = currentEmbedding(a, emb);
  ai::StyleResult res;
  std::string err;
  if (!aiStyleSolveFile(*r, sets[a.styles.selected], a.path, has ? emb : nullptr, a.params, a.styleGroups, res, err)) {
    showToast(a, std::string(tr(T_AiUnavailable)) + ": " + err);
    return;
  }
  a.styleOpt = res.params;
  a.styleValid = true;
  a.params = ai::blendStyle(a.params, a.styleOpt, a.styleStrength / 100.f);
  storeCropUser(a);
  a.er.changed = true;
  a.er.commit = T_AiStyle;
  a.er.extra = sets[a.styles.selected].name;
}

static void drawAiStylePanel(App& a) {
  const auto& sets = a.styles.sets();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (sets.empty()) {
    uiHint(tr(T_NoStyles));
  } else {
    int& sel = a.styles.selected;
    sel = std::clamp(sel, 0, (int)sets.size() - 1);
    char label[96];
    snprintf(label, sizeof(label), "%s (%zu)", sets[sel].name.c_str(), sets[sel].refs.size());
    if (ImGui::BeginCombo("##style", label)) {
      for (int i = 0; i < (int)sets.size(); i++)
        if (ImGui::Selectable(sets[i].name.c_str(), i == sel)) sel = i, a.styleValid = false;
      ImGui::EndCombo();
    }
  }
  if (a.styles.creating()) {
    ImGui::ProgressBar(a.styles.progress(), ImVec2(-FLT_MIN, 0), tr(T_Analyzing));
  } else if (ImGui::Button(trId(T_NewStyle, "newstyle"), ImVec2(-FLT_MIN, 0))) {
    a.openNewStyle = true;
    snprintf(a.newStyleName, sizeof(a.newStyleName), "Style %d", (int)sets.size() + 1);
  }
  const int keys[5] = {T_AdjLight, T_AdjColor, T_AdjHsl, T_AdjGrade, T_AdjCurve};
  float lineW = 0, availW = ImGui::GetContentRegionAvail().x;
  for (int i = 0; i < 5; i++) {
    float bw = ImGui::CalcTextSize(tr(keys[i])).x + 18.f * a.scale + ImGui::GetStyle().ItemSpacing.x;
    if (i && lineW + bw < availW) ImGui::SameLine();
    else if (i) lineW = 0;
    lineW += bw;
    ImGui::PushID(i);
    bool on = (a.styleGroups >> i) & 1;
    if (uiPill(tr(keys[i]), on)) a.styleGroups ^= 1u << i;
    ImGui::PopID();
  }
  float before = a.styleStrength;
  EditResult sr;
  uiSlider(sr, T_Strength, "aistr", &a.styleStrength, 0, 100, 100, "%.0f");
  if (a.styleValid && a.styleStrength != before) {  // re-blend live, no new solve
    a.params = ai::blendStyle(a.params, a.styleOpt, a.styleStrength / 100.f);
    a.er.changed = true;
  }
  if (a.styleValid && sr.commit >= 0) a.er.commit = T_AiStyle;
  ImGui::BeginDisabled(sets.empty() || !a.styleGroups);
  if (uiPrimaryButton(trId(T_Apply, "aiapply"))) applyAiStyle(a);
  ImGui::EndDisabled();
}

static const int kRegionKeys[ai::REG_COUNT] = {T_Reg_Person, T_Reg_Sky,      T_Reg_Road,    T_Reg_Building, T_Reg_Trees,
                                               T_Reg_Grass,  T_Reg_Water,    T_Reg_Mountain, T_Reg_Ground,  T_Reg_Vehicle,
                                               T_Reg_Animal, T_Reg_Walls,    T_Reg_Food,    T_Reg_Face};

static void resetMaskKeys(App& a) {
  for (int i = 0; i < kMaxLayers; i++) {
    a.chanRegion[i] = -2;
    a.chanInvert[i] = 0;
    a.chanFeather[i] = -1.f;
  }
}

static void clearSegmentation(App& a) {
  a.seg = ai::SegResult();
  std::vector<float>().swap(a.guide);
  std::vector<uint8_t>().swap(a.maskRGBA);
  a.segPath.clear();
  a.R.clearMask();
  resetMaskKeys(a);
  a.maskVer++;
}

static void startSegmentation(App& a) {
  if (!a.hasImage || a.segJob.active || a.segPath == a.path) return;
  a.segJob.active = true;
  a.segJob.done = false;
  a.segJob.path = a.path;
  a.segJob.th = std::thread([&J = a.segJob]() {
    try {
      J.ok = ai::segmentFile(J.path, J.seg, J.guide, J.W, J.H, J.err);
    } catch (const std::exception& e) {
      J.ok = false;
      J.err = e.what();
    }
    J.done = true;
    glfwPostEmptyEvent();
  });
}

static void finishSegmentation(App& a) {
  a.segJob.th.join();
  a.segJob.active = false;
  if (a.segJob.path != a.path) return;  // another photo was opened meanwhile
  a.segPath = a.path;  // do not retry automatically after a failure
  if (!a.segJob.ok) {
    showToast(a, std::string(tr(T_AiUnavailable)) + ": " + a.segJob.err);
    return;
  }
  a.seg = std::move(a.segJob.seg);
  a.guide.swap(a.segJob.guide);
  std::vector<float>().swap(a.segJob.guide);
  a.maskW = a.segJob.W;
  a.maskH = a.segJob.H;
  a.maskRGBA.assign((size_t)a.maskW * a.maskH * 4, 0);
  resetMaskKeys(a);
}

// Rebuilds only the mask channels whose region / invert / feather changed.
static void updateMasks(App& a) {
  if (!a.seg.valid() || a.maskRGBA.empty()) return;
  bool changed = false;
  for (int i = 0; i < kMaxLayers; i++) {
    const LocalLayer& l = a.params.layers[i];
    int reg = l.enabled ? l.region : -1;
    if (reg == a.chanRegion[i] && l.invert == a.chanInvert[i] && l.feather == a.chanFeather[i]) continue;
    if (reg >= 0) {
      ai::buildLayerMask(a.seg, reg, l.invert != 0, l.feather, a.guide, a.maskW, a.maskH, a.maskRGBA.data(), i);
    } else {
      for (size_t k = i; k < a.maskRGBA.size(); k += 4) a.maskRGBA[k] = 0;
    }
    a.chanRegion[i] = reg;
    a.chanInvert[i] = l.invert;
    a.chanFeather[i] = l.feather;
    changed = true;
  }
  if (changed) {
    a.R.setMask(a.maskRGBA.data(), a.maskW, a.maskH);
    a.maskVer++;
  }
}

static void addLayer(App& a, int region, bool invert) {
  for (int i = 0; i < kMaxLayers; i++) {
    LocalLayer& l = a.params.layers[i];
    if (l.enabled) continue;
    std::memset((void*)&l, 0, sizeof(l));
    l.enabled = 1;
    l.region = region;
    l.invert = invert ? 1 : 0;
    l.feather = 10.f;
    a.er.changed = true;
    a.er.commit = T_LocalLayer;
    a.er.extra = invert ? tr(T_Background) : tr(kRegionKeys[region]);
    return;
  }
  showToast(a, tr(T_MaxLayers));
}

static void drawLocalPanel(App& a) {
  EditParams& p = a.params;
  uiCardBegin("##regions", tr(T_FoundRegions), false);
  if (!a.seg.valid()) {
    startSegmentation(a);
    if (a.segJob.active) {
      uiHint(tr(T_Segmenting));
    } else if (uiPrimaryButton(trId(T_DetectRegions, "segnow"))) {
      a.segPath.clear();
      startSegmentation(a);
    }
  } else {
    uiHint(tr(T_RegionHint));
    ImGui::Dummy(ImVec2(0, 2.f * a.scale));
    int order[ai::REG_COUNT];
    for (int r = 0; r < ai::REG_COUNT; r++) order[r] = r;
    std::sort(order, order + ai::REG_COUNT, [&](int x, int y) { return a.seg.area[x] > a.seg.area[y]; });
    float lineW = 0, avail = ImGui::GetContentRegionAvail().x;
    auto chip = [&](const char* label) {
      float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2 + 6;
      if (lineW > 0 && lineW + w < avail) ImGui::SameLine();
      else lineW = 0;
      lineW += w;
      return uiPill(label, false);
    };
    if (!a.seg.faces.empty()) {  // faces first: often under 1% of the frame, but the most wanted
      char label[96];
      snprintf(label, sizeof(label), "%s %d", tr(T_Reg_Face), (int)a.seg.faces.size());
      ImGui::PushID("face");
      if (chip(label)) addLayer(a, ai::REG_FACE, false);
      ImGui::PopID();
    }
    for (int k = 0; k < ai::REG_COUNT; k++) {
      int r = order[k];
      if (r == ai::REG_FACE) continue;
      if (a.seg.area[r] < 0.005f) break;
      char label[96];
      snprintf(label, sizeof(label), "%s %.0f%%", tr(kRegionKeys[r]), a.seg.area[r] * 100.f);
      ImGui::PushID(r);
      if (chip(label)) addLayer(a, r, false);
      ImGui::PopID();
    }
    if (a.seg.area[ai::REG_PERSON] >= 0.01f) {
      ImGui::PushID("bg");
      if (chip(tr(T_Background))) addLayer(a, ai::REG_PERSON, true);
      ImGui::PopID();
    }
  }
  uiCardEnd();
  bool any = false;
  for (int i = 0; i < kMaxLayers; i++) {
    LocalLayer& l = p.layers[i];
    if (!l.enabled) continue;
    any = true;
    ImGui::PushID(1000 + i);
    std::string name = l.region >= 0 && l.region < ai::REG_COUNT ? tr(kRegionKeys[l.region]) : "?";
    if (l.invert) name = l.region == ai::REG_PERSON ? std::string(tr(T_Background)) : name + " (" + tr(T_Invert) + ")";
    uiCardBegin("##layer", name.c_str(), false);
    if (uiPill(trId(T_Invert, "inv"), l.invert != 0)) {
      l.invert ^= 1;
      a.er.changed = true;
      a.er.commit = T_LocalLayer;
    }
    ImGui::SameLine();
    bool remove = uiPill(trId(T_Remove, "rm"), false, IM_COL32(255, 140, 120, 255));
    ImGui::Dummy(ImVec2(0, 2.f * a.scale));
    uiSlider(a.er, T_Exposure, "le", &l.exposure, -3, 3, 0, "%+.2f");
    uiSlider(a.er, T_Contrast, "lc", &l.contrast, -100, 100, 0, "%.0f");
    uiSlider(a.er, T_Highlights, "lh", &l.highlights, -100, 100, 0, "%.0f");
    uiSlider(a.er, T_Shadows, "ls", &l.shadows, -100, 100, 0, "%.0f");
    uiSlider(a.er, T_Temp, "lt", &l.temp, -100, 100, 0, "%.0f", IM_COL32(70, 120, 235, 255), IM_COL32(235, 200, 60, 255));
    uiSlider(a.er, T_Tint, "lti", &l.tint, -100, 100, 0, "%.0f", IM_COL32(70, 200, 80, 255), IM_COL32(215, 70, 200, 255));
    uiSlider(a.er, T_Saturation, "lsa", &l.saturation, -100, 100, 0, "%.0f", IM_COL32(128, 128, 128, 255),
             IM_COL32(230, 60, 60, 255));
    uiSlider(a.er, T_Clarity, "lcl", &l.clarity, -100, 100, 0, "%.0f");
    uiSlider(a.er, T_Feather, "lf", &l.feather, 0, 100, 10, "%.0f");
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) a.hoverLayer = i;
    uiCardEnd();
    if (remove) {
      std::memset((void*)&l, 0, sizeof(l));
      a.er.changed = true;
      a.er.commit = T_Remove;
    }
    ImGui::PopID();
  }
  if (any) uiHint(tr(T_MaskHint));
}

// ---------------------------------------------------------------- photo layers (compositing)
static void addImageLayer(App& a, const std::string& path) {
  if (!a.hasImage || path.empty()) return;
  if (path.size() >= (size_t)kLayerPathMax) {
    showToast(a, tr(T_PathTooLong));
    return;
  }
  std::string why;
  if (!layerPathAllowed(path, why)) {
    showToast(a, std::string(tr(T_LoadFailed)) + ": " + fileName(path) + " (" + why + ")");
    return;
  }
  int slot = -1;
  for (int i = 0; i < kMaxImgLayers && slot < 0; i++)
    if (!a.params.img[i].enabled) slot = i;
  if (slot < 0) {
    showToast(a, tr(T_MaxImgLayers));
    return;
  }
  // Fit inside 70% of the frame, whatever the two aspect ratios are.
  float scale = 0.7f;
  ai::SmallImage probe;
  ai::DecodeInfo info;
  std::string err;
  if (ai::decodeScaled(path, 64, probe, &info, err) && info.origW > 0) {
    float la = (float)info.origH / info.origW, ba = (float)a.R.orientedH() / std::max(1, a.R.orientedW());
    scale = std::min(0.7f, 0.7f * ba / la);
  } else if (!err.empty()) {
    showToast(a, std::string(tr(T_LoadFailed)) + ": " + fileName(path));
    return;
  }
  // Keep the stack compact: the new layer goes on top of the existing ones.
  ImageLayer l;
  std::memset((void*)&l, 0, sizeof(l));
  l.enabled = 1;
  l.id = a.layers.nextId(a.params);
  l.cx = l.cy = 0.5f;
  l.scale = scale;
  l.opacity = 100.f;
  l.feather = 5.f;
  std::memcpy(l.path, path.data(), path.size());
  int n = 0;
  ImageLayer tmp[kMaxImgLayers];
  std::memset((void*)tmp, 0, sizeof(tmp));
  for (const ImageLayer& e : a.params.img)
    if (e.enabled) tmp[n++] = e;
  tmp[n] = l;
  std::memcpy(a.params.img, tmp, sizeof(tmp));
  a.selLayer = n;
  a.layerTool = 0;
  a.er.changed = true;
  a.er.commit = T_AddLayer;
  a.er.extra = fileName(path);
}

// Layer uv <-> output uv (0..1 of the shown image) for the current geometry.
static bool layerMaps(App& a, const Geometry& g, const ImageLayer& l, M3& layerToOut, M3& outToLayer) {
  int lw, lh;
  a.R.layerSize(l.id, lw, lh);
  if (lw <= 0) return false;
  M3 lo = layerToOriented(l, a.R.orientedW(), a.R.orientedH(), lw, lh);
  layerToOut = invertM3(g.outToOriented);
  float m[9];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      float s = 0;
      for (int k = 0; k < 3; k++) s += layerToOut.m[i * 3 + k] * lo.m[k * 3 + j];
      m[i * 3 + j] = s;
    }
  std::memcpy(layerToOut.m, m, sizeof(m));
  outToLayer = invertM3(layerToOut);
  return true;
}

// Visible at layer uv (u, v)? Uses the cut-out / brush mask, so clicks pass through empty parts.
static bool layerHit(App& a, const ImageLayer& l, float u, float v) {
  if (u < 0 || v < 0 || u > 1 || v > 1) return false;
  LayerAsset* A = a.layers.asset(l.id);
  if (!A || !A->maskOn || A->mw <= 0) return true;
  int x = std::min(A->mw - 1, (int)(u * A->mw)), y = std::min(A->mh - 1, (int)(v * A->mh));
  return A->alpha[(size_t)y * A->mw + x] > 0.15f;
}

// Handles, outline and mouse work for the selected layer. Returns true while it owns the mouse.
static void layerCanvas(App& a, const Geometry& g, ImVec2 tl, float dW, float dH, bool hovered, bool active) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float s = a.scale;
  EditParams& p = a.params;
  auto toScreen = [&](const M3& lo, float u, float v) {
    float x, y;
    applyM3(lo, u, v, x, y);
    return ImVec2(tl.x + x * dW, tl.y + y * dH);
  };
  auto mouseOut = [&](float& x, float& y) {
    x = (io.MousePos.x - tl.x) / dW;
    y = (io.MousePos.y - tl.y) / dH;
  };
  // Mouse in oriented pixels of the base photo: moves and angles are measured there.
  auto mouseOriented = [&](float& x, float& y) {
    float ox, oy;
    mouseOut(ox, oy);
    applyM3(g.outToOriented, ox, oy, x, y);
    x *= a.R.orientedW();
    y *= a.R.orientedH();
  };
  if (a.selLayer >= 0 && (a.selLayer >= kMaxImgLayers || !p.img[a.selLayer].enabled)) a.selLayer = -1;

  // Outlines of every layer, the selected one with handles.
  int hoverHandle = 0;  // 1 inside, 2 corner, 3 rotate
  for (int i = 0; i < kMaxImgLayers; i++) {
    ImageLayer& l = p.img[i];
    M3 lo, ol;
    if (!l.enabled || l.hidden || !layerMaps(a, g, l, lo, ol)) continue;
    ImVec2 c[4] = {toScreen(lo, 0, 0), toScreen(lo, 1, 0), toScreen(lo, 1, 1), toScreen(lo, 0, 1)};
    bool sel = i == a.selLayer;
    if (!sel) {
      dl->AddPolyline(c, 4, IM_COL32(240, 235, 225, 70), ImDrawFlags_Closed, 1.f);
      continue;
    }
    dl->AddPolyline(c, 4, IM_COL32(0, 0, 0, 120), ImDrawFlags_Closed, 3.f);
    dl->AddPolyline(c, 4, pal::kAccent, ImDrawFlags_Closed, 1.5f);
    if (a.layerTool != 0) continue;
    ImVec2 top((c[0].x + c[1].x) * 0.5f, (c[0].y + c[1].y) * 0.5f), mid((c[0].x + c[2].x) * 0.5f, (c[0].y + c[2].y) * 0.5f);
    float ux = top.x - mid.x, uy = top.y - mid.y, ul = std::max(1e-3f, std::sqrt(ux * ux + uy * uy));
    ImVec2 rot(top.x + ux / ul * 26.f * s, top.y + uy / ul * 26.f * s);
    dl->AddLine(top, rot, pal::kAccent, 1.5f);
    dl->AddCircleFilled(rot, 6.f * s, pal::kText);
    dl->AddCircle(rot, 6.f * s, pal::kAccent, 0, 1.5f);
    float hs = 5.f * s;
    for (const ImVec2& q : c) {
      dl->AddRectFilled(ImVec2(q.x - hs, q.y - hs), ImVec2(q.x + hs, q.y + hs), pal::kText);
      dl->AddRect(ImVec2(q.x - hs, q.y - hs), ImVec2(q.x + hs, q.y + hs), pal::kAccent, 0, 0, 1.5f);
    }
    if (hovered && a.layerDrag == 0) {
      auto near = [&](ImVec2 q, float r) {
        float dx = io.MousePos.x - q.x, dy = io.MousePos.y - q.y;
        return dx * dx + dy * dy <= r * r;
      };
      float ox, oy, u, v;
      mouseOut(ox, oy);
      applyM3(ol, ox, oy, u, v);
      if (near(rot, 10.f * s)) hoverHandle = 3;
      else if (near(c[0], 10.f * s) || near(c[1], 10.f * s) || near(c[2], 10.f * s) || near(c[3], 10.f * s)) hoverHandle = 2;
      else if (u >= 0 && v >= 0 && u <= 1 && v <= 1) hoverHandle = 1;
    }
  }

  // Magic-eraser strokes: the one being painted and those AI is still filling, in red.
  {
    M3 orToOut = invertM3(g.outToOriented);
    auto orScreen = [&](float u, float v) {
      float x, y;
      applyM3(orToOut, u, v, x, y);
      return ImVec2(tl.x + x * dW, tl.y + y * dH);
    };
    ImVec2 e0 = orScreen(0, 0), e1 = orScreen(1, 0);
    float owScreen = std::hypot(e1.x - e0.x, e1.y - e0.y);
    auto drawStroke = [&](const HealStroke& st, ImU32 col) {
      float rad = std::max(1.f, st.radius * owScreen);
      for (size_t k = 0; k + 1 < st.pts.size(); k += 2) {
        ImVec2 q = orScreen(st.pts[k], st.pts[k + 1]);
        dl->AddCircleFilled(q, rad, col, 24);
        if (k >= 2) {
          ImVec2 q0 = orScreen(st.pts[k - 2], st.pts[k - 1]);
          dl->AddLine(q0, q, col, rad * 2.f);
        }
      }
    };
    const auto& hs = a.heal.strokes();
    bool busyShown = false;
    for (int k = 0; k < std::min(p.heals, (int)hs.size()); k++)
      if (!a.heal.ready(k)) {
        drawStroke(hs[k], IM_COL32(226, 82, 54, 110));
        if (!busyShown && !hs[k].pts.empty()) {
          ImVec2 q = orScreen(hs[k].pts[0], hs[k].pts[1]);
          const char* msg = tr(T_MagicBusy);
          ImVec2 ts = ImGui::CalcTextSize(msg);
          dl->AddRectFilled(ImVec2(q.x - ts.x * 0.5f - 10, q.y - ts.y - 30), ImVec2(q.x + ts.x * 0.5f + 10, q.y - 16),
                            IM_COL32(18, 17, 16, 225), 3.f);
          dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y - 23), pal::kText, msg);
          busyShown = true;
        }
      }
    if (a.layerDrag == 5) drawStroke(a.healPend, IM_COL32(226, 82, 54, 140));
    // Press / drag / release of the magic eraser
    if (a.layerTool == 3) {
      float ox, oy, u, v;
      mouseOut(ox, oy);
      applyM3(g.outToOriented, ox, oy, u, v);
      if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) {
        a.healPend = HealStroke();
        a.healPend.radius = a.brushPx * 0.5f / std::max(1.f, owScreen);
        a.healPend.pts = {u, v};
        a.layerDrag = 5;
      } else if (a.layerDrag == 5 && active) {
        float dx = u - a.healPend.pts[a.healPend.pts.size() - 2], dy = v - a.healPend.pts.back();
        if (dx * dx + dy * dy > 1e-8f && a.healPend.pts.size() < kMaxStrokePoints * 2) {
          a.healPend.pts.push_back(u);
          a.healPend.pts.push_back(v);
        }
      }
      if (a.layerDrag == 5 && !active) {
        a.heal.add(p, a.R, std::move(a.healPend));
        a.healPend = HealStroke();
        a.layerDrag = 0;
        a.er.changed = true;
        a.er.commit = T_ToolMagic;
      }
    }
  }

  // Brush cursor
  bool brush = a.layerTool == 3 || (a.layerTool != 0 && a.selLayer >= 0);
  if (brush && (hovered || a.layerDrag == 4)) {
    dl->AddCircle(io.MousePos, a.brushPx * 0.5f, IM_COL32(0, 0, 0, 160), 0, 3.f);
    dl->AddCircle(io.MousePos, a.brushPx * 0.5f, a.layerTool == 1 ? pal::kText : pal::kAccent, 0, 1.5f);
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
  } else if (a.layerDrag == 1 || hoverHandle == 1) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  } else if (a.layerDrag == 2 || hoverHandle == 2) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
  } else if (a.layerDrag == 3 || hoverHandle == 3) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  // Press: grab a handle, start a stroke, or pick the layer under the mouse.
  if (a.layerTool != 3 && ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) {
    mouseOriented(a.dragX, a.dragY);
    if (brush) {
      ImageLayer& l = p.img[a.selLayer];
      M3 lo, ol;
      if (layerMaps(a, g, l, lo, ol)) {
        float ox, oy, u, v;
        mouseOut(ox, oy);
        applyM3(ol, ox, oy, u, v);
        ImVec2 e0 = toScreen(lo, 0, 0), e1 = toScreen(lo, 1, 0);
        float lwScreen = std::max(1.f, std::sqrt((e1.x - e0.x) * (e1.x - e0.x) + (e1.y - e0.y) * (e1.y - e0.y)));
        a.layers.beginStroke(l, a.layerTool == 1 ? 1 : 0, a.brushPx * 0.5f / lwScreen, a.brushHard / 100.f, u, v);
        a.layerDrag = 4;
        a.er.changed = true;
      }
    } else if (hoverHandle >= 2 && a.selLayer >= 0) {
      a.layerDrag = hoverHandle;
      a.layerStart = p.img[a.selLayer];
    } else {
      int hit = -1;
      for (int i = kMaxImgLayers - 1; i >= 0 && hit < 0; i--) {
        const ImageLayer& l = p.img[i];
        M3 lo, ol;
        if (!l.enabled || l.hidden || !layerMaps(a, g, l, lo, ol)) continue;
        float ox, oy, u, v;
        mouseOut(ox, oy);
        applyM3(ol, ox, oy, u, v);
        if (layerHit(a, l, u, v) || (i == a.selLayer && hoverHandle == 1)) hit = i;
      }
      if (hit >= 0) {
        a.selLayer = hit;
        a.layerDrag = 1;
        a.layerStart = p.img[hit];
      }
    }
  }

  // Drag
  if (a.layerDrag && a.layerDrag != 5 && active && a.selLayer >= 0) {
    ImageLayer& l = p.img[a.selLayer];
    float mx, my;
    mouseOriented(mx, my);
    float Wo = (float)a.R.orientedW(), Ho = (float)a.R.orientedH();
    float cx = a.layerStart.cx * Wo, cy = a.layerStart.cy * Ho;
    if (a.layerDrag == 1) {
      l.cx = a.layerStart.cx + (mx - a.dragX) / Wo;
      l.cy = a.layerStart.cy + (my - a.dragY) / Ho;
    } else if (a.layerDrag == 2) {
      float d0 = std::hypot(a.dragX - cx, a.dragY - cy), d1 = std::hypot(mx - cx, my - cy);
      if (d0 > 1.f) l.scale = std::clamp(a.layerStart.scale * d1 / d0, 0.01f, 20.f);
    } else if (a.layerDrag == 3) {
      float a0 = std::atan2(a.dragY - cy, a.dragX - cx), a1 = std::atan2(my - cy, mx - cx);
      float deg = a.layerStart.rotate + (a1 - a0) * 180.f / 3.14159265f;
      deg = std::fmod(deg + 540.f, 360.f) - 180.f;
      if (io.KeyShift) deg = std::round(deg / 15.f) * 15.f;
      l.rotate = deg;
    } else if (a.layerDrag == 4) {
      M3 lo, ol;
      if (layerMaps(a, g, l, lo, ol)) {
        float ox, oy, u, v;
        mouseOut(ox, oy);
        applyM3(ol, ox, oy, u, v);
        a.layers.extendStroke(l, u, v);
      }
    }
    a.er.changed = true;
  }
  if (a.layerDrag && a.layerDrag != 5 && !active) {
    a.er.commit = a.layerDrag == 4 ? T_LayerBrush : T_LayerEdit;
    a.layerDrag = 0;
  }
}

static const char* layerStatus(App& a, const ImageLayer& l) {
  LayerAsset* A = a.layers.asset(l.id);
  if (!A) return tr(T_LayerLoading);
  int st = A->stage;
  if (st == -1) return tr(T_LayerFailed);
  if (st < 2) return tr(T_LayerLoading);
  if (st < 4) return tr(T_LayerAi);
  return nullptr;
}

static void drawLayersPanel(App& a) {
  EditParams& p = a.params;
  float s = a.scale;
  if (a.selLayer >= 0 && (a.selLayer >= kMaxImgLayers || !p.img[a.selLayer].enabled)) a.selLayer = -1;
  if ((a.layerTool == 1 || a.layerTool == 2) && a.selLayer < 0) a.layerTool = 0;
  if (a.layerTool == 3) {
    uiCardBegin("##magic", tr(T_MagicTitle), false);
    uiHint(tr(T_MagicHint));
    ImGui::Dummy(ImVec2(0, 2.f * s));
    EditResult dummy;
    uiSlider(dummy, T_BrushSize, "msz", &a.brushPx, 4, 800, 60, "%.0f px");
    if (a.heal.busy(p)) {
      ImGui::PushStyleColor(ImGuiCol_Text, pal::kAccent);
      ImGui::TextUnformatted(tr(T_MagicBusy));
      ImGui::PopStyleColor();
    }
    if (p.heals > 0) {
      char buf[96];
      snprintf(buf, sizeof(buf), tr(T_MagicCount), p.heals);
      ImGui::TextUnformatted(buf);
      ImGui::SameLine();
      if (uiPill(trId(T_MagicClear, "mclr"), false)) {
        p.heals = 0;
        a.er.changed = true;
        a.er.commit = T_MagicClear;
      }
    }
    uiCardEnd();
  }
  uiCardBegin("##imglayers", tr(T_PhotoLayers), false);
  if (uiPrimaryButton(trId(T_AddPhotoLayer, "addlayer"))) {
    for (const std::string& f : openImagesDialog(glfwGetWin32Window(a.win), a.S.lastDir)) addImageLayer(a, f);
  }
  uiHint(tr(T_LayersHint));
  ImGui::Dummy(ImVec2(0, 4.f * s));
  int count = 0;
  for (const ImageLayer& l : p.img) count += l.enabled ? 1 : 0;
  if (!count) uiHint(tr(T_NoLayers));
  // Top of the stack first, like every layers panel.
  float rowH = 44.f * s, w = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  int removeAt = -1;
  for (int i = kMaxImgLayers - 1; i >= 0; i--) {
    ImageLayer& l = p.img[i];
    if (!l.enabled) continue;
    ImGui::PushID(2000 + l.id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool sel = i == a.selLayer;
    if (ImGui::InvisibleButton("##row", ImVec2(w - 56.f * s, rowH))) a.selLayer = i;
    bool hov = ImGui::IsItemHovered();
    if (sel || hov) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rowH), sel ? pal::kPanel : IM_COL32(255, 255, 255, 10));
    if (sel) dl->AddRectFilled(p0, ImVec2(p0.x + 2.f * s, p0.y + rowH), pal::kAccent);
    LayerAsset* A = a.layers.asset(l.id);
    float th = rowH - 8.f * s, tw = th;
    if (A && A->thumbTex && A->th2 > 0) {
      tw = std::min(th * 1.5f, th * A->tw / A->th2);
      dl->AddImage((ImTextureID)(intptr_t)A->thumbTex, ImVec2(p0.x + 8.f * s, p0.y + 4.f * s),
                   ImVec2(p0.x + 8.f * s + tw, p0.y + 4.f * s + th));
    } else {
      dl->AddRect(ImVec2(p0.x + 8.f * s, p0.y + 4.f * s), ImVec2(p0.x + 8.f * s + tw, p0.y + 4.f * s + th), pal::kFrame);
    }
    float tx = p0.x + 16.f * s + std::max(tw, th);
    std::string name = fileName(l.path);
    dl->PushClipRect(ImVec2(tx, p0.y), ImVec2(p0.x + w - 60.f * s, p0.y + rowH), true);
    dl->AddText(ImVec2(tx, p0.y + 5.f * s), l.hidden ? pal::kFaint : pal::kText, name.c_str());
    const char* st = layerStatus(a, l);
    char sub[96];
    if (st) snprintf(sub, sizeof(sub), "%s", st);
    else snprintf(sub, sizeof(sub), "%.0f%%%s", l.opacity, l.keep ? "  \xc2\xb7  AI" : "");
    ImGui::PushFont(g_fontSmall);
    dl->AddText(ImVec2(tx, p0.y + rowH - ImGui::GetFontSize() - 6.f * s), st ? pal::kAccent : pal::kMuted, sub);
    ImGui::PopFont();
    dl->PopClipRect();
    ImGui::SameLine(0, 0);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 54.f * s, p0.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
    if (uiPill(l.hidden ? trId(T_Show, "vis") : trId(T_Hide, "vis"), false)) {
      l.hidden ^= 1;
      a.er.changed = true;
      a.er.commit = l.hidden ? T_Hide : T_Show;
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH + 2.f * s));
    ImGui::PopID();
  }
  if (count) {  // the base photo sits under every layer
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##baserow", ImVec2(w, rowH * 0.7f))) a.selLayer = -1;
    dl->AddText(ImVec2(p0.x + 8.f * s, p0.y + (rowH * 0.7f - ImGui::GetFontSize()) * 0.5f), pal::kMuted, tr(T_BasePhoto));
  }
  uiCardEnd();
  if (a.selLayer < 0) return;

  int i = a.selLayer;
  ImageLayer& l = p.img[i];
  LayerAsset* A = a.layers.asset(l.id);
  uiCardBegin("##layertools", tr(T_SelectedLayer), false);
  if (a.layerTool != 1 && a.layerTool != 2) {
    uiHint(tr(T_MoveHint));
  } else {
    EditResult dummy;
    uiSlider(dummy, T_BrushSize, "bsz", &a.brushPx, 4, 800, 60, "%.0f px");
    uiSlider(dummy, T_BrushHard, "bhd", &a.brushHard, 0, 100, 50, "%.0f");
    uiHint(tr(T_BrushHint));
  }
  ImGui::Dummy(ImVec2(0, 2.f * s));
  auto swapWith = [&](int j) {
    std::swap(p.img[i], p.img[j]);
    a.selLayer = j;
    a.er.changed = true;
    a.er.commit = T_LayerEdit;
  };
  int above = -1, below = -1;
  for (int j = i + 1; j < kMaxImgLayers && above < 0; j++)
    if (p.img[j].enabled) above = j;
  for (int j = i - 1; j >= 0 && below < 0; j--)
    if (p.img[j].enabled) below = j;
  ImGui::BeginDisabled(above < 0);
  if (uiPill(trId(T_LayerUp, "lup"), false)) swapWith(above);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(below < 0);
  if (uiPill(trId(T_LayerDown, "ldn"), false)) swapWith(below);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (uiPill(trId(T_Flip, "lflip"), l.flipH != 0)) {
    l.flipH ^= 1;
    a.er.changed = true;
    a.er.commit = T_Flip;
  }
  ImGui::SameLine();
  if (uiPill(trId(T_ResetPlace, "lctr"), false)) {
    l.cx = l.cy = 0.5f;
    l.rotate = 0.f;
    a.er.changed = true;
    a.er.commit = T_LayerEdit;
  }
  if (uiPill(trId(T_RemoveLayer, "lrm"), false, IM_COL32(255, 140, 120, 255))) removeAt = i;
  uiCardEnd();

  uiCardBegin("##cutout", tr(T_CutOut), false);
  if (!A || A->stage == -1) {
    uiHint(tr(T_LayerFailed));
  } else if (A->stage != 4) {
    uiHint(tr(T_CutBusy));
  } else if (!A->segOk) {
    uiHint(tr(T_CutNone));
  } else {
    const ai::SegResult& sg = A->seg;
    if (uiPrimaryButton(trId(T_CutAuto, "cutauto"))) {
      // The subject: people (with their faces), else the largest animal / vehicle / food.
      int keep = 0;
      if (sg.area[ai::REG_PERSON] >= 0.02f) keep = 1 << ai::REG_PERSON;
      else {
        int best = -1;
        for (int r : {ai::REG_ANIMAL, ai::REG_VEHICLE, ai::REG_FOOD})
          if (sg.area[r] >= 0.02f && (best < 0 || sg.area[r] > sg.area[best])) best = r;
        if (best >= 0) keep = 1 << best;
      }
      if (!keep) keep = 1 << ai::REG_PERSON;
      l.keep = keep;
      a.er.changed = true;
      a.er.commit = T_CutOut;
    }
    uiHint(tr(T_CutHint));
    ImGui::Dummy(ImVec2(0, 2.f * s));
    float lineW = 0, avail = ImGui::GetContentRegionAvail().x;
    auto chip = [&](const char* label, bool on) {
      float cw = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2 + 6;
      if (lineW > 0 && lineW + cw < avail) ImGui::SameLine();
      else lineW = 0;
      lineW += cw;
      return uiPill(label, on);
    };
    ImGui::PushID("cw");
    if (chip(tr(T_CutWhole), l.keep == 0)) {
      l.keep = 0;
      a.er.changed = true;
      a.er.commit = T_CutOut;
    }
    ImGui::PopID();
    int order[ai::REG_COUNT];
    for (int r = 0; r < ai::REG_COUNT; r++) order[r] = r;
    std::sort(order, order + ai::REG_COUNT, [&](int x, int y) { return sg.area[x] > sg.area[y]; });
    for (int k = 0; k < ai::REG_COUNT; k++) {
      int r = order[k];
      if (r == ai::REG_FACE || sg.area[r] < 0.005f) continue;
      char label[96];
      snprintf(label, sizeof(label), "%s %.0f%%", tr(kRegionKeys[r]), sg.area[r] * 100.f);
      ImGui::PushID(r);
      if (chip(label, (l.keep >> r) & 1)) {
        l.keep ^= 1 << r;
        a.er.changed = true;
        a.er.commit = T_CutOut;
      }
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 2.f * s));
    uiSlider(a.er, T_EdgeSoft, "lfe", &l.feather, 0, 100, 5, "%.0f");
  }
  uiCardEnd();

  uiCardBegin("##lblend", tr(T_Blending), false);
  uiSlider(a.er, T_Opacity, "lop", &l.opacity, 0, 100, 100, "%.0f%%");
  static const int kBlendKeys[BLEND_COUNT] = {T_BlNormal, T_BlMultiply, T_BlScreen, T_BlOverlay,
                                              T_BlSoft,   T_BlDarken,   T_BlLighten};
  ImGui::TextUnformatted(tr(T_BlendMode));
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##blend", tr(kBlendKeys[std::clamp(l.blend, 0, BLEND_COUNT - 1)]))) {
    for (int m = 0; m < BLEND_COUNT; m++)
      if (ImGui::Selectable(tr(kBlendKeys[m]), m == l.blend)) {
        l.blend = m;
        a.er.changed = true;
        a.er.commit = T_BlendMode;
      }
    ImGui::EndCombo();
  }
  uiCardEnd();

  uiCardBegin("##lmatch", tr(T_MatchLook), false);
  ImGui::BeginDisabled(!A || !A->uploaded);
  if (uiPrimaryButton(trId(T_MatchAuto, "lmauto"))) {
    if (a.layers.matchColours(a.path, l)) {
      a.er.changed = true;
      a.er.commit = T_MatchLook;
    }
  }
  ImGui::EndDisabled();
  uiSlider(a.er, T_Exposure, "lex", &l.exposure, -3, 3, 0, "%+.2f");
  uiSlider(a.er, T_Temp, "ltp", &l.temp, -100, 100, 0, "%.0f", IM_COL32(70, 120, 235, 255), IM_COL32(235, 200, 60, 255));
  uiSlider(a.er, T_Saturation, "lsat", &l.saturation, -100, 100, 0, "%.0f", IM_COL32(128, 128, 128, 255),
           IM_COL32(230, 60, 60, 255));
  uiCardEnd();

  if (removeAt >= 0) {
    std::string nm = fileName(p.img[removeAt].path);
    for (int j = removeAt; j + 1 < kMaxImgLayers; j++) p.img[j] = p.img[j + 1];
    std::memset((void*)&p.img[kMaxImgLayers - 1], 0, sizeof(ImageLayer));
    a.selLayer = -1;
    a.er.changed = true;
    a.er.commit = T_RemoveLayer;
    a.er.extra = nm;
  }
}

static void clearRecover(App& a) {
  a.R.clearRecover();
  a.recPath.clear();
  a.maskVer++;
}

static void startRecover(App& a) {
  if (!a.hasImage || a.recJob.active) return;
  a.recJob.active = true;
  a.recJob.done = false;
  a.recJob.path = a.path;
  a.recJob.segOk = a.seg.valid();
  if (a.recJob.segOk) a.recJob.seg = a.seg;  // regions split white sky from white sand
  a.recJob.th = std::thread([&J = a.recJob]() {
    try {
      J.ok = ai::recoverFile(J.path, J.segOk ? &J.seg : nullptr, true, J.rec, J.err);
    } catch (const std::exception& e) {
      J.ok = false;
      J.err = e.what();
    }
    J.done = true;
    glfwPostEmptyEvent();
  });
}

static void finishRecover(App& a) {
  a.recJob.th.join();
  a.recJob.active = false;
  ai::Recovery rec = std::move(a.recJob.rec);
  a.recJob.rec = ai::Recovery();
  a.recJob.seg = ai::SegResult();
  if (a.recJob.path != a.path) return;
  if (!a.recJob.ok) {
    showToast(a, std::string(tr(T_AiUnavailable)) + ": " + a.recJob.err);
    return;
  }
  if (!rec.valid() || (rec.areas == 0 && rec.blown == 0)) {
    if (!ai::loadRecoverCache(a.path, rec) || !rec.valid()) {
      showToast(a, tr(T_NoBlown));
      return;
    }
  }
  a.R.setRecover(rec.rgba.data(), rec.w, rec.h);
  a.recPath = a.path;
  a.maskVer++;
  if (a.params.recover <= 0) {
    a.params.recover = 100.f;
    a.hist.push(a.params, T_Recover);
    saveSidecar(a);
  }
  char msg[128];
  if (rec.ms > 0)
    snprintf(msg, sizeof(msg), "%s %.1f%% %s", tr(T_Recovered), rec.blown * 100.f, tr(T_OfImage));
  else
    snprintf(msg, sizeof(msg), "%s", tr(T_Recovered));
  showToast(a, msg);
}

static float blownFromHistogram(const App& a) {
  unsigned total = 0, white = 0;
  for (int i = 0; i < 256; i++) total += a.histo[3][i];
  white = a.histo[3][255] + a.histo[3][254];
  return total ? (float)white / total : 0.f;
}

static void drawRecoverPanel(App& a) {
  float blown = blownFromHistogram(a);
  char info[96];
  snprintf(info, sizeof(info), "%s: %.1f%%", tr(T_BlownShare), blown * 100.f);
  uiHint(info);
  // A small meter: how much of the photo is pure white.
  {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 4.f * a.scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), pal::kTrack, h);
    float f = std::min(1.f, blown * 5.f);  // 20% white fills the bar
    if (f > 0) dl->AddRectFilled(p0, ImVec2(p0.x + std::max(h, w * f), p0.y + h), IM_COL32(255, 120, 100, 255), h);
    ImGui::Dummy(ImVec2(w, h + 4.f * a.scale));
  }
  if (a.recJob.active) {
    ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), ImVec2(-FLT_MIN, 0), tr(T_Recovering));
  } else if (uiPrimaryButton(trId(T_RecoverNow, "recnow"))) {
    startRecover(a);
  }
  if (a.R.hasRecover()) uiSlider(a.er, T_Strength, "recstr", &a.params.recover, 0, 100, 100, "%.0f");
  uiHint(tr(T_RecoverHint));
  if (a.R.hasRecover() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
    a.hoverLayer = 4;  // show the rebuilt areas
}

static EditParams blendAuto(const App& a, float strength) {
  EditParams p = ai::blendStyle(a.params, a.autoOpt, strength / 100.f);
  copyLayers(p, a.autoOpt);  // region layers come with the auto result
  return p;
}

// Blocking (~1-3 s, models load once): scene + target + regions + CLIP-ranked variants.
static void applyAutoEdit(App& a) {
  if (!a.hasImage) return;
  Renderer* r = aiRenderer(a);
  if (!r) return;
  AutoModels m;
  std::string err;
  if (!m.load(err)) {
    showToast(a, std::string(tr(T_AiUnavailable)) + ": " + err);
    return;
  }
  float emb[ai::kEmbDim];
  bool has = aiCachedEmbedding(a.path, emb);
  bool segOk = a.seg.valid();
  ai::AutoResult res;
  bool ok = aiAutoEditFile(*r, m, a.path, has ? emb : nullptr, segOk ? &a.seg : nullptr, segOk ? &a.guide : nullptr,
                           a.maskW, a.maskH, true, a.params, res, err);
  m.release();
  ai::ortUnload();
  if (!ok) {
    showToast(a, std::string(tr(T_AiUnavailable)) + ": " + err);
    return;
  }
  a.autoOpt = res.params;
  a.autoValid = true;
  a.autoScene = res.scene;
  a.autoConf = res.sceneConf;
  a.params = blendAuto(a, a.autoStrength);
  storeCropUser(a);
  a.er.changed = true;
  a.er.commit = T_AutoEdit;
  a.er.extra = sceneName(res.scene);
  if (anyLayer(a.params) && !a.seg.valid()) {
    a.segPath.clear();
    startSegmentation(a);  // the editor needs the regions to draw the new layers
  }
  if (blownFromHistogram(a) > 0.005f && !a.R.hasRecover()) {
    a.autoOpt.recover = a.params.recover = 100.f;
    startRecover(a);
  }
}

static void drawAutoEdit(App& a) {
  ImGui::BeginDisabled(!a.hasImage);
  uiHint(tr(T_AutoHint));
  ImGui::Dummy(ImVec2(0, 2.f * a.scale));
  if (uiPrimaryButton(trId(T_AutoEdit, "autoedit"))) applyAutoEdit(a);
  if (a.autoValid) {
    char sc[128];
    snprintf(sc, sizeof(sc), "%s: %s (%.0f%%)", tr(T_Scene), sceneName(a.autoScene), a.autoConf * 100.f);
    uiHint(sc);
    float before = a.autoStrength;
    EditResult sr;
    uiSlider(sr, T_Strength, "autostr", &a.autoStrength, 0, 100, 100, "%.0f");
    if (a.autoStrength != before) {
      a.params = blendAuto(a, a.autoStrength);
      a.er.changed = true;
    }
    if (sr.commit >= 0) a.er.commit = T_AutoEdit;
  }
  ImGui::EndDisabled();
}

// ---------------------------------------------------------------- crop helpers
static float aspectValue(int idx, int Wf, int Hf) {
  switch (idx) {
    case 1: return (float)Wf / Hf;
    case 2: return 1.f;
    case 3: return 4.f / 5.f;
    case 4: return 5.f / 4.f;
    case 5: return 3.f / 2.f;
    case 6: return 2.f / 3.f;
    case 7: return 16.f / 9.f;
    case 8: return 9.f / 16.f;
    default: return 0.f;
  }
}

static void applyAspect(App& a) {
  int Wf, Hf;
  frameOf(a, a.params, Wf, Hf);
  float A = aspectValue(a.aspectIdx, Wf, Hf);
  if (A <= 0.f) return;
  EditParams& p = a.params;
  float wn, hn;
  if (A >= (float)Wf / Hf) {
    wn = 1.f;
    hn = Wf / A / Hf;
  } else {
    hn = 1.f;
    wn = Hf * A / Wf;
  }
  float cx = p.cropX + p.cropW * 0.5f, cy = p.cropY + p.cropH * 0.5f;
  p.cropW = wn;
  p.cropH = hn;
  p.cropX = std::clamp(cx - wn * 0.5f, 0.f, 1.f - wn);
  p.cropY = std::clamp(cy - hn * 0.5f, 0.f, 1.f - hn);
  if (a.constrain) constrainCrop(p, Wf, Hf);
  storeCropUser(a);
  a.er.changed = true;
  a.er.commit = T_Aspect;
}

static void cropOverlay(App& a, ImVec2 tl, float dW, float dH, bool active) {
  EditParams& p = a.params;
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  int Wf, Hf;
  frameOf(a, p, Wf, Hf);
  float A = aspectValue(a.aspectIdx, Wf, Hf);
  ImVec2 m = io.MousePos;
  float x0 = tl.x + p.cropX * dW, y0 = tl.y + p.cropY * dH, x1 = x0 + p.cropW * dW, y1 = y0 + p.cropH * dH;
  float hr = 10.f * a.scale;
  auto nearPt = [&](float px, float py) { return std::fabs(m.x - px) < hr && std::fabs(m.y - py) < hr; };
  int hit = -1;  // 0..3 corners TL TR BL BR, 4..7 edges L R T B, 8 inside
  if (nearPt(x0, y0)) hit = 0;
  else if (nearPt(x1, y0)) hit = 1;
  else if (nearPt(x0, y1)) hit = 2;
  else if (nearPt(x1, y1)) hit = 3;
  else if (std::fabs(m.x - x0) < hr * 0.7f && m.y > y0 && m.y < y1) hit = 4;
  else if (std::fabs(m.x - x1) < hr * 0.7f && m.y > y0 && m.y < y1) hit = 5;
  else if (std::fabs(m.y - y0) < hr * 0.7f && m.x > x0 && m.x < x1) hit = 6;
  else if (std::fabs(m.y - y1) < hr * 0.7f && m.x > x0 && m.x < x1) hit = 7;
  else if (m.x > x0 && m.x < x1 && m.y > y0 && m.y < y1) hit = 8;
  if (A > 0.f && hit >= 4 && hit <= 7) hit = -1;  // edges are free-aspect only

  int shown = a.cropHandle >= 0 ? a.cropHandle : (ImGui::IsItemHovered() ? hit : -1);
  if (shown == 0 || shown == 3) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
  else if (shown == 1 || shown == 2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
  else if (shown == 4 || shown == 5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  else if (shown == 6 || shown == 7) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  else if (shown == 8) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) {
    a.cropHandle = hit;
    a.cropStart = p;
    a.cropMouse = m;
  }
  if (active && a.cropHandle >= 0 && ImGui::IsMouseDown(0)) {
    EditParams prev = p;
    float dx = (m.x - a.cropMouse.x) / dW, dy = (m.y - a.cropMouse.y) / dH;
    const EditParams& s = a.cropStart;
    float L = s.cropX, T = s.cropY, Rr = s.cropX + s.cropW, B = s.cropY + s.cropH;
    int h = a.cropHandle;
    if (h == 8) {
      float w = Rr - L, hh = B - T;
      L = std::clamp(L + dx, 0.f, 1.f - w);
      T = std::clamp(T + dy, 0.f, 1.f - hh);
      Rr = L + w;
      B = T + hh;
    } else {
      bool left = h == 0 || h == 2 || h == 4, right = h == 1 || h == 3 || h == 5;
      bool top = h == 0 || h == 1 || h == 6, bottom = h == 2 || h == 3 || h == 7;
      const float mn = 0.02f;
      if (left) L = std::clamp(L + dx, 0.f, Rr - mn);
      if (right) Rr = std::clamp(Rr + dx, L + mn, 1.f);
      if (top) T = std::clamp(T + dy, 0.f, B - mn);
      if (bottom) B = std::clamp(B + dy, T + mn, 1.f);
      if (A > 0.f && h < 4) {  // height follows width, anchored at the opposite corner
        float hn = (Rr - L) * Wf / A / Hf;
        float maxH = top ? B : 1.f - T;
        if (hn > maxH) {
          hn = maxH;
          float wn = hn * Hf * A / Wf;
          if (left) L = Rr - wn;
          else Rr = L + wn;
        }
        if (top) T = B - hn;
        else B = T + hn;
      }
    }
    p.cropX = L;
    p.cropY = T;
    p.cropW = Rr - L;
    p.cropH = B - T;
    if (a.constrain && !cropFits(p, Wf, Hf)) copyGeometry(p, prev);  // stop at the image edge
    a.er.changed = true;
  }
  if (ImGui::IsItemDeactivated() && a.cropHandle >= 0) {
    a.cropHandle = -1;
    storeCropUser(a);
    a.er.commit = T_Crop;
  }

  x0 = tl.x + p.cropX * dW;
  y0 = tl.y + p.cropY * dH;
  x1 = x0 + p.cropW * dW;
  y1 = y0 + p.cropH * dH;
  ImU32 shade = IM_COL32(0, 0, 0, 150);
  ImVec2 i0 = tl, i1(tl.x + dW, tl.y + dH);
  dl->AddRectFilled(i0, ImVec2(i1.x, y0), shade);
  dl->AddRectFilled(ImVec2(i0.x, y1), i1, shade);
  dl->AddRectFilled(ImVec2(i0.x, y0), ImVec2(x0, y1), shade);
  dl->AddRectFilled(ImVec2(x1, y0), ImVec2(i1.x, y1), shade);
  for (int i = 1; i < 3; i++) {
    float fx = x0 + (x1 - x0) * i / 3.f, fy = y0 + (y1 - y0) * i / 3.f;
    dl->AddLine(ImVec2(fx, y0), ImVec2(fx, y1), IM_COL32(255, 255, 255, 70));
    dl->AddLine(ImVec2(x0, fy), ImVec2(x1, fy), IM_COL32(255, 255, 255, 70));
  }
  dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 220), 0, 0, 1.5f);
  float hl = 14.f * a.scale, th = 3.f * a.scale;
  ImU32 hc = pal::kAccent;
  dl->AddRectFilled(ImVec2(x0 - th, y0 - th), ImVec2(x0 + hl, y0), hc);
  dl->AddRectFilled(ImVec2(x0 - th, y0 - th), ImVec2(x0, y0 + hl), hc);
  dl->AddRectFilled(ImVec2(x1 - hl, y0 - th), ImVec2(x1 + th, y0), hc);
  dl->AddRectFilled(ImVec2(x1, y0 - th), ImVec2(x1 + th, y0 + hl), hc);
  dl->AddRectFilled(ImVec2(x0 - th, y1), ImVec2(x0 + hl, y1 + th), hc);
  dl->AddRectFilled(ImVec2(x0 - th, y1 - hl), ImVec2(x0, y1 + th), hc);
  dl->AddRectFilled(ImVec2(x1 - hl, y1), ImVec2(x1 + th, y1 + th), hc);
  dl->AddRectFilled(ImVec2(x1, y1 - hl), ImVec2(x1 + th, y1 + th), hc);
  char dims[64];
  snprintf(dims, sizeof(dims), "%d x %d", (int)std::lround(p.cropW * Wf), (int)std::lround(p.cropH * Hf));
  dl->AddText(ImVec2(x0 + 6, y1 - ImGui::GetFontSize() - 6), IM_COL32(255, 255, 255, 200), dims);
}

// ---------------------------------------------------------------- UI pieces
enum Page { PG_AI, PG_LIGHT, PG_COLOR, PG_DETAIL, PG_EFFECTS, PG_LOCAL, PG_LAYERS, PG_CROP, PG_LOOKS, PG_HISTORY, PG_COUNT };

static void setPage(App& a, int pg) {
  if (pg == a.page) return;
  if (a.page == PG_CROP) a.cropMode = false;
  if (pg == PG_CROP && a.hasImage) {
    a.cropMode = true;
    a.view.fit = true;
  }
  a.page = pg;
}

static void setLanguage(App& a, int lang) {
  setLang(lang ? Lang::TH : Lang::EN);
  a.S.lang = lang;
}

// ---------------------------------------------------------------- text size
static const int kTextSizes[] = {85, 100, 115, 130, 150};
static const int kTextSizeCount = (int)(sizeof(kTextSizes) / sizeof(kTextSizes[0]));

static int clampTextPct(int p) {
  int best = 100, bd = 1 << 30;
  for (int v : kTextSizes)
    if (std::abs(v - p) < bd) bd = std::abs(v - p), best = v;
  return best;
}

static void requestTextSize(App& a, int pct) {
  pct = clampTextPct(pct);
  if (pct != a.S.textPct) a.pendingTextPct = pct;
}

static void stepTextSize(App& a, int dir) {
  int i = 0;
  while (i < kTextSizeCount && kTextSizes[i] != a.S.textPct) i++;
  i = std::clamp(i + dir, 0, kTextSizeCount - 1);
  requestTextSize(a, kTextSizes[i]);
}

static void setupFonts(float scale);

// Rebuilds the font atlas and the style for the new size. Must run outside NewFrame/Render.
static void applyTextSize(App& a) {
  if (!a.pendingTextPct) return;
  a.S.textPct = a.pendingTextPct;
  a.pendingTextPct = 0;
  a.scale = a.dpiScale * a.S.textPct / 100.f;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  setupFonts(a.scale);
  ImGui_ImplOpenGL3_DestroyFontsTexture();
  ImGui_ImplOpenGL3_CreateFontsTexture();
  ImGui::GetStyle() = ImGuiStyle();  // applyTheme scales from the defaults, so start from them
  applyTheme(a.scale);
  char msg[64];
  snprintf(msg, sizeof(msg), "%s %d%%", tr(T_TextSize), a.S.textPct);
  showToast(a, msg);
  saveSettings(a.S);
}

static void tourGo(App& a, int step);

// Vertically centres the next widget of height h inside a bar of height barH.
static void centerY(float barH, float h) { ImGui::SetCursorPosY(std::floor((barH - h) * 0.5f)); }

static void drawTopBar(App& a, float barH) {
  float s = a.scale;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 w0 = ImGui::GetWindowPos();
  float W = ImGui::GetWindowWidth();
  float fh = ImGui::GetFrameHeight();

  // Wordmark: the mark, then "Circle" in paper white and "Room" in the lamp's red.
  float x = 16.f * s, cy = w0.y + barH * 0.5f;
  float mk = 30.f * s;
  drawIcon(dl, IC_LOGO, ImVec2(w0.x + x + mk * 0.5f, cy), mk, 0);
  x += mk + 10.f * s;
  ImGui::PushFont(g_fontTitle);
  ImVec2 t1 = ImGui::CalcTextSize("Circle "), t2 = ImGui::CalcTextSize("Room");
  float ty = cy - t1.y * 0.5f - 1.f * s;
  dl->AddText(ImVec2(w0.x + x, ty), pal::kText, "Circle");
  dl->AddText(ImVec2(w0.x + x + t1.x, ty), pal::kAccent, "Room");
  ImGui::PopFont();
  x += t1.x + t2.x + 30.f * s;

  // Mode switch: Edit is a plain tab; AI Cull is a lit button so nobody can miss it.
  ImGui::SetCursorPos(ImVec2(x, std::floor((barH - fh) * 0.5f)));
  bool composing = a.mode == 0 && a.page == PG_LAYERS;
  if (uiTextToggle(tr(T_ModeEdit), a.mode == 0 && !composing)) {
    a.mode = 0;
    if (composing) setPage(a, a.editPage == PG_LAYERS ? PG_LIGHT : a.editPage);
  }
  ImGui::SameLine(0, 12.f * s);
  {  // Compose: a main mode of its own, not just a page in the index
    float bh = fh + 6.f * s;
    centerY(barH, bh);
    ImGui::PushFont(g_fontBold);
    const char* cl = tr(T_ModeCompose);
    float tw = ImGui::CalcTextSize(cl).x;
    float iw = 16.f * s, bw = 14.f * s + iw + 8.f * s + tw + 16.f * s;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##composemode", ImVec2(bw, bh));
    bool hov = ImGui::IsItemHovered();
    ImVec2 p1(p0.x + bw, p0.y + bh);
    if (composing) dl->AddRectFilled(p0, p1, pal::kText, 3.f * s);
    else {
      if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 18), 3.f * s);
      dl->AddRect(p0, p1, pal::kText, 3.f * s, 0, 1.5f);
    }
    ImU32 fg = composing ? pal::kInk : pal::kText;
    drawIcon(dl, IC_LAYERS, ImVec2(p0.x + 14.f * s + iw * 0.5f, p0.y + bh * 0.5f), iw, fg);
    dl->AddText(ImVec2(p0.x + 14.f * s + iw + 8.f * s, p0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), fg, cl);
    ImGui::PopFont();
    if (hov) ImGui::SetTooltip("%s", tr(T_ComposeTip));
    if (clicked) {
      a.mode = 0;
      if (a.page != PG_LAYERS) {
        a.editPage = a.page;
        setPage(a, PG_LAYERS);
      }
    }
  }
  ImGui::SameLine(0, 12.f * s);
  {
    float bh = fh + 6.f * s;
    centerY(barH, bh);
    ImGui::PushFont(g_fontBold);
    const char* cl = tr(T_ModeCull);
    float tw = ImGui::CalcTextSize(cl).x;
    float iw = 16.f * s, bw = 14.f * s + iw + 8.f * s + tw + 16.f * s;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##cullmode", ImVec2(bw, bh));
    bool hov = ImGui::IsItemHovered(), on = a.mode == 1;
    tourSetItem(TR_CULLTAB);
    ImVec2 p1(p0.x + bw, p0.y + bh);
    if (on) {
      dl->AddRectFilled(p0, p1, pal::kAccent, 3.f * s);
    } else {
      dl->AddRectFilled(p0, p1, hov ? IM_COL32(226, 82, 54, 70) : pal::kAccentSoft, 3.f * s);
      dl->AddRect(p0, p1, pal::kAccent, 3.f * s, 0, 1.5f);
    }
    ImU32 fg = on ? pal::kOnAccent : pal::kAccent;
    drawIcon(dl, IC_AI, ImVec2(p0.x + 14.f * s + iw * 0.5f, p0.y + bh * 0.5f), iw, fg);
    dl->AddText(ImVec2(p0.x + 14.f * s + iw + 8.f * s, p0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), fg, cl);
    ImGui::PopFont();
    if (hov) ImGui::SetTooltip("%s", tr(T_Tr7B));
    if (clicked) a.mode = 1;
  }

  float ib = fh + 2.f * s;
  if (a.mode == 0) {
    ImGui::SameLine(0, 26.f * s);
    float lx = ImGui::GetCursorScreenPos().x;
    dl->AddLine(ImVec2(lx - 13.f * s, cy - 9.f * s), ImVec2(lx - 13.f * s, cy + 9.f * s), pal::kFrame, 1.f);
    centerY(barH, ib);
    if (iconButton("##open", IC_OPEN, false, tr(T_Open), ib)) doOpenDialog(a);
    tourSetItem(TR_OPEN);
    ImGui::SameLine(0, 2.f * s);
    centerY(barH, ib);
    ImGui::BeginDisabled(!a.hist.canUndo());
    if (iconButton("##undo", IC_UNDO, false, tr(T_Undo), ib)) doUndo(a);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2.f * s);
    centerY(barH, ib);
    ImGui::BeginDisabled(!a.hist.canRedo());
    if (iconButton("##redo", IC_REDO, false, tr(T_Redo), ib)) doRedo(a);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2.f * s);
    centerY(barH, ib);
    ImGui::BeginDisabled(!a.hasImage || isDefault(a.params));
    if (iconButton("##reset", IC_RESET, false, tr(T_Reset), ib)) {
      a.params = EditParams();
      storeCropUser(a);
      a.er.changed = true;
      a.er.commit = T_Reset;
    }
    ImGui::EndDisabled();
  }

  // Right side: language, then export
  const char* langs[2] = {"EN", "TH"};
  float langW = ImGui::CalcTextSize("EN").x + ImGui::CalcTextSize("TH").x + 18.f * s;
  float right = W - 20.f * s - langW;
  ImGui::SetCursorPos(ImVec2(right, std::floor((barH - fh) * 0.5f)));
  int l = uiSegmented("lang", langs, 2, currentLang() == Lang::TH ? 1 : 0, false);
  if (l >= 0) setLanguage(a, l);
  // Help: replays the tutorial
  float hb = fh;
  float hx = right - 14.f * s - hb;
  ImGui::SetCursorPos(ImVec2(hx, std::floor((barH - fh) * 0.5f)));
  {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##help", ImVec2(hb, hb));
    bool hov = ImGui::IsItemHovered();
    tourSetItem(TR_HELP);
    ImVec2 c(p0.x + hb * 0.5f, p0.y + hb * 0.5f);
    dl->AddCircle(c, hb * 0.42f, hov ? pal::kText : pal::kTrack, 24, 1.5f);
    ImGui::PushFont(g_fontBold);
    ImVec2 q = ImGui::CalcTextSize("?");
    dl->AddText(ImVec2(c.x - q.x * 0.5f, c.y - q.y * 0.5f), hov ? pal::kText : pal::kMuted, "?");
    ImGui::PopFont();
    if (hov) ImGui::SetTooltip("%s", tr(T_TourReplay));
    if (clicked) tourGo(a, 0);
  }
  // Text size: "Aa" opens a small panel with the sizes
  float ax = hx - 8.f * s - hb;
  ImGui::SetCursorPos(ImVec2(ax, std::floor((barH - fh) * 0.5f)));
  {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##textsize", ImVec2(hb, hb));
    bool hov = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen("##textsizepop");
    ImU32 col = hov || open ? pal::kText : pal::kMuted;
    ImGui::PushFont(g_fontBold);
    ImVec2 q = ImGui::CalcTextSize("Aa");
    dl->AddText(ImVec2(p0.x + (hb - q.x) * 0.5f, p0.y + (hb - q.y) * 0.5f - 1.f * s), col, "Aa");
    ImGui::PopFont();
    if (open) dl->AddRectFilled(ImVec2(p0.x + 4.f * s, p0.y + hb - 2.f * s), ImVec2(p0.x + hb - 4.f * s, p0.y + hb), pal::kAccent);
    if (hov && !open) ImGui::SetTooltip("%s  (Ctrl +/-)", tr(T_TextSize));
    if (clicked) ImGui::OpenPopup("##textsizepop");
    ImGui::SetNextWindowPos(ImVec2(p0.x + hb, p0.y + hb + 8.f * s), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f * s, 14.f * s));
    if (ImGui::BeginPopup("##textsizepop")) {
      ImGui::PushFont(g_fontBold);
      ImGui::TextUnformatted(tr(T_TextSize));
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(0, 2.f * s));
      const char* labels[kTextSizeCount];
      char buf[kTextSizeCount][8];
      int sel = 1;
      for (int i = 0; i < kTextSizeCount; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%d%%", kTextSizes[i]);
        labels[i] = buf[i];
        if (kTextSizes[i] == (a.pendingTextPct ? a.pendingTextPct : a.S.textPct)) sel = i;
      }
      ImGui::PushFont(g_fontMono);
      ImGui::SetNextItemWidth(320.f * s);
      ImGui::Dummy(ImVec2(320.f * s, 0));
      int k = uiSegmented("sizes", labels, kTextSizeCount, sel);
      ImGui::PopFont();
      if (k >= 0) requestTextSize(a, kTextSizes[k]);
      ImGui::Dummy(ImVec2(0, 4.f * s));
      ImGui::TextUnformatted(tr(T_TextSizePreview));
      uiHint(tr(T_TextSizeHint));
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
  }
  if (a.mode == 0) {
    float expW = std::max(104.f * s, ImGui::CalcTextSize(tr(T_Export)).x + 52.f * s);
    float ex = ax - 18.f * s - expW;
    ImGui::SetCursorPos(ImVec2(ex, std::floor((barH - fh) * 0.5f)));
    if (a.exp.stage != 0) {
      float prog = a.exp.stage == 1 ? a.R.exportProgress() * 0.8f : 0.9f;
      ImGui::ProgressBar(prog, ImVec2(expW, fh), tr(T_Exporting));
    } else {
      ImGui::BeginDisabled(!a.hasImage);
      ImVec2 bp = ImGui::GetCursorScreenPos();
      if (uiPrimaryButton(trId(T_Export, "export"), ImVec2(expW, fh))) a.openExport = true;
      tourSetItem(TR_EXPORT);
      drawIcon(dl, IC_EXPORT, ImVec2(bp.x + expW - 18.f * s, bp.y + fh * 0.5f), fh * 0.36f,
               a.hasImage ? pal::kInk : pal::kFaint);
      ImGui::EndDisabled();
    }
  }
}

static const int kPageLabels[PG_COUNT] = {T_PgAI,    T_PgLight, T_PgColor, T_PgDetail, T_PgEffects,
                                          T_PgLocal, T_PgLayers, T_PgCrop, T_PgLooks, T_PgHistory};

static void drawRail(App& a) {
  float s = a.scale;
  tourSetWindow(TR_RAIL);
  // Rows shrink (down to the text height) when large text sizes on a short window would push
  // the last pages off the bottom.
  float sp = ImGui::GetStyle().ItemSpacing.y;
  float fixedH = 14.f * s + sp + 2 * (12.f * s + sp);  // top gap + the two group breaks
  float w = ImGui::GetContentRegionAvail().x;
  float h = std::clamp((ImGui::GetWindowHeight() - fixedH - 8.f * s) / PG_COUNT - sp, ImGui::GetTextLineHeight() + 6.f * s,
                       36.f * s);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::Dummy(ImVec2(0, 14.f * s));
  for (int i = 0; i < PG_COUNT; i++) {
    if (i == PG_LOCAL || i == PG_LOOKS) {  // group breaks
      ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddLine(ImVec2(p.x + 18.f * s, p.y + 6.f * s), ImVec2(p.x + 34.f * s, p.y + 6.f * s), pal::kFrame, 1.f);
      ImGui::Dummy(ImVec2(0, 12.f * s));
    }
    ImGui::PushID(i);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##rail", ImVec2(w, h))) setPage(a, i);
    bool hov = ImGui::IsItemHovered(), sel = a.page == i;
    ImGui::PopID();
    if (sel) {
      dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), pal::kPanel);
      dl->AddRectFilled(p0, ImVec2(p0.x + 2.f * s, p0.y + h), pal::kAccent);
    }
    char num[4];
    snprintf(num, sizeof(num), "%02d", i + 1);
    ImGui::PushFont(g_fontMono);
    ImVec2 ns = ImGui::CalcTextSize(num);
    dl->AddText(ImVec2(p0.x + 18.f * s, p0.y + (h - ns.y) * 0.5f), sel ? pal::kAccent : pal::kFaint, num);
    ImGui::PopFont();
    const char* label = tr(kPageLabels[i]);
    ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + 18.f * s + ns.x + 10.f * s, p0.y + (h - ls.y) * 0.5f - 1.f * s),
                sel ? pal::kText : (hov ? pal::kText : pal::kMuted), label);
  }
  // Shortcut hint at the foot of the index, only when it doesn't collide with the last entry
  // (large text sizes on short windows leave no room).
  float y = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - 30.f * s;
  if (y > ImGui::GetCursorScreenPos().y + 8.f * s) {
    ImGui::PushFont(g_fontSmall);
    dl->AddText(ImVec2(ImGui::GetWindowPos().x + 18.f * s, y), pal::kFaint, tr(T_RailHint));
    ImGui::PopFont();
  }
}

static void drawLooksPage(App& a) {
  const Preset* hovered = nullptr;
  uiHint(tr(T_LooksHint));
  ImGui::Dummy(ImVec2(0, 2.f * a.scale));
  uiCardBegin("##looks", nullptr, false);
  bool userHeader = false;
  float rowH = ImGui::GetFrameHeight() + 4.f * a.scale;
  for (size_t i = 0; i < a.presets.size(); i++) {
    const Preset& pr = a.presets[i];
    if (!pr.builtin && !userHeader) {
      ImGui::Dummy(ImVec2(0, 4.f * a.scale));
      uiHint(tr(T_UserPresets));
      userHeader = true;
    }
    const char* name = (currentLang() == Lang::TH && !pr.nameTh.empty()) ? pr.nameTh.c_str() : pr.nameEn.c_str();
    ImGui::PushID((int)i);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##look", ImVec2(w, rowH))) {
      applyPreset(pr, a.params);
      a.er.changed = true;
      a.er.commit = T_Preset;
      a.er.extra = name;
    }
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rowH), pal::kHover, 7.f * a.scale);
    float r = 5.f * a.scale;
    dl->AddCircleFilled(ImVec2(p0.x + 12.f * a.scale, p0.y + rowH * 0.5f), r, hov ? pal::kAccent : pal::kFaint);
    dl->AddText(ImVec2(p0.x + 26.f * a.scale, p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                hov ? pal::kText : pal::kMuted, name);
    if (hov) hovered = &pr;
    ImGui::PopID();
  }
  uiCardEnd();
  if (ImGui::Button(trId(T_SavePreset, "savep"), ImVec2(-FLT_MIN, 0))) {
    a.openSavePreset = true;
    a.presetName[0] = 0;
  }
  // Hovering a look previews it live on the photo.
  a.hoverActive = hovered != nullptr && !a.cropMode;
  if (a.hoverActive) {
    a.hoverParams = a.params;
    applyPreset(*hovered, a.hoverParams);
  }
}

static void drawHistoryPage(App& a) {
  uiCardBegin("##hist", nullptr, false);
  const auto& es = a.hist.entries();
  float rowH = ImGui::GetFrameHeight();
  for (int i = (int)es.size() - 1; i >= 0; i--) {
    std::string label = tr(es[i].label);
    if (!es[i].extra.empty()) label += ": " + es[i].extra;
    ImGui::PushID(i);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##h", ImVec2(w, rowH))) {
      a.params = a.hist.jump(i);
      paramsReplaced(a);
    }
    bool hov = ImGui::IsItemHovered(), cur = i == a.hist.index(), future = i > a.hist.index();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (cur) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rowH), pal::kAccentSoft, 7.f * a.scale);
    else if (hov) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rowH), pal::kHover, 7.f * a.scale);
    // timeline rail
    float cx = p0.x + 10.f * a.scale, cy = p0.y + rowH * 0.5f;
    if (i > 0) dl->AddLine(ImVec2(cx, cy), ImVec2(cx, p0.y + rowH + ImGui::GetStyle().ItemSpacing.y), pal::kFrame, 2.f);
    dl->AddCircleFilled(ImVec2(cx, cy), 4.f * a.scale, cur ? pal::kAccent : (future ? pal::kFaint : pal::kMuted));
    dl->AddText(ImVec2(p0.x + 24.f * a.scale, p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                cur ? pal::kText : (future ? pal::kFaint : pal::kMuted), label.c_str());
    ImGui::PopID();
  }
  uiCardEnd();
}

static void drawCropPanel(App& a) {
  EditParams& p = a.params;
  int Wf, Hf;
  frameOf(a, p, Wf, Hf);
  float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  if (a.cropMode) {
    if (uiPrimaryButton(trId(T_Done, "cropmode"), ImVec2(half, 0))) a.cropMode = false;
  } else if (ImGui::Button(trId(T_EditCrop, "cropmode"), ImVec2(half, 0))) {
    a.cropMode = true;
    a.view.fit = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(trId(T_ResetCrop, "rcrop"), ImVec2(-FLT_MIN, 0))) {
    p.cropX = p.cropY = 0;
    p.cropW = p.cropH = 1;
    p.straighten = 0;
    storeCropUser(a);
    a.er.changed = true;
    a.er.commit = T_ResetCrop;
  }
  ImGui::Dummy(ImVec2(0, 2.f * a.scale));
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
  ImGui::TextUnformatted(tr(T_Aspect));
  ImGui::PopStyleColor();
  ImGui::SetNextItemWidth(-FLT_MIN);
  const char* items[9] = {tr(T_Free), tr(T_Original), "1 : 1", "4 : 5", "5 : 4", "3 : 2", "2 : 3", "16 : 9", "9 : 16"};
  if (ImGui::Combo("##aspect", &a.aspectIdx, items, 9)) applyAspect(a);
  ImGui::Dummy(ImVec2(0, 2.f * a.scale));

  float before = p.straighten;
  EditResult sr;
  uiSlider(sr, T_Straighten, "str", &p.straighten, -45.f, 45.f, 0.f, "%.1f°");
  if (p.straighten != before) {  // re-fit the crop from the user's crop, so it can grow back
    p.cropX = a.cropUser[0];
    p.cropY = a.cropUser[1];
    p.cropW = a.cropUser[2];
    p.cropH = a.cropUser[3];
    if (a.constrain) constrainCrop(p, Wf, Hf);
    a.er.changed = true;
  }
  if (sr.commit >= 0) a.er.commit = T_Straighten;

  ImGui::Dummy(ImVec2(0, 2.f * a.scale));
  int rot = 0;
  if (ImGui::Button(trId(T_RotateLeft, "rl"), ImVec2(half, 0))) rot = 3;
  ImGui::SameLine();
  if (ImGui::Button(trId(T_RotateRight, "rr"), ImVec2(-FLT_MIN, 0))) rot = 1;
  if (rot) {
    p.rotate90 = (p.rotate90 + rot) & 3;
    p.cropX = p.cropY = 0;
    p.cropW = p.cropH = 1;
    frameOf(a, p, Wf, Hf);
    if (a.constrain) constrainCrop(p, Wf, Hf);
    storeCropUser(a);
    a.er.changed = true;
    a.er.commit = T_Rotate;
  }
  if (ImGui::Button(trId(T_FlipH, "fh"), ImVec2(half, 0))) {
    p.flipH ^= 1;
    p.cropX = 1.f - p.cropX - p.cropW;
    p.straighten = -p.straighten;
    a.cropUser[0] = 1.f - a.cropUser[0] - a.cropUser[2];
    a.er.changed = true;
    a.er.commit = T_Flip;
  }
  ImGui::SameLine();
  if (ImGui::Button(trId(T_FlipV, "fv"), ImVec2(-FLT_MIN, 0))) {
    p.flipV ^= 1;
    p.cropY = 1.f - p.cropY - p.cropH;
    p.straighten = -p.straighten;
    a.cropUser[1] = 1.f - a.cropUser[1] - a.cropUser[3];
    a.er.changed = true;
    a.er.commit = T_Flip;
  }
  ImGui::Dummy(ImVec2(0, 2.f * a.scale));
  if (ImGui::Checkbox(trId(T_Constrain, "constrain"), &a.constrain) && a.constrain) {
    constrainCrop(p, Wf, Hf);
    a.er.changed = true;
    a.er.commit = T_Crop;
  }
}

static void updateHistogram(App& a) {
  const EditParams& shown = a.hoverActive ? a.hoverParams : a.params;
  if (a.hasImage && (!a.histValid || !sameParams(shown, a.histParams))) {
    a.R.histogram(shown, a.histo);
    a.histParams = shown;
    a.histValid = true;
  }
}

static void drawToolPanel(App& a) {
  static const int titles[PG_COUNT] = {T_TitleAI,    T_TitleLight, T_TitleColor, T_TitleDetail,  T_TitleEffects,
                                       T_TitleLocal, T_TitleLayers, T_TitleCrop, T_TitleLooks, T_TitleHistory};
  float s = a.scale;
  tourSetWindow(TR_TOOLS);
  a.hoverLayer = -1;
  a.hoverActive = false;
  a.localPanelOpen = a.page == PG_LOCAL;
  uiSectionReset();
  ImGui::Dummy(ImVec2(0, 6.f * s));
  {
    char idx[16];
    snprintf(idx, sizeof(idx), "%02d / %02d", a.page + 1, (int)PG_COUNT);
    ImGui::PushFont(g_fontMono);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kFaint);
    ImGui::TextUnformatted(idx);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted(tr(titles[a.page]));
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(0, 8.f * s));
  // Always reserve the scrollbar: otherwise wrapped text can make it appear/disappear every frame.
  ImGui::BeginChild("##pagebody", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
  ImGui::BeginDisabled(!a.hasImage);
  EditParams& p = a.params;
  switch (a.page) {
    case PG_AI:
      uiCardBegin("##auto", tr(T_AutoEdit), false);
      drawAutoEdit(a);
      uiCardEnd();
      uiCardBegin("##recover", tr(T_Recover), false);
      drawRecoverPanel(a);
      uiCardEnd();
      uiCardBegin("##aistyle", tr(T_AiStyle), false);
      drawAiStylePanel(a);
      uiCardEnd();
      break;
    case PG_LIGHT: {
      bool r = uiCardBegin("##light", tr(T_CardLight));
      uiLight(p, a.er, r);
      uiCardEnd();
      r = uiCardBegin("##curve", tr(T_ToneCurve));
      uiToneCurve(p, a.er, a.ps, a.histo, s, r);
      uiCardEnd();
      break;
    }
    case PG_COLOR: {
      bool r = uiCardBegin("##wb", tr(T_WhiteBalance));
      uiWhiteBalance(p, a.er, r);
      uiCardEnd();
      r = uiCardBegin("##vivid", tr(T_CardVivid));
      uiVividness(p, a.er, r);
      uiCardEnd();
      r = uiCardBegin("##hsl", tr(T_ColorMixer));
      uiColorMixer(p, a.er, a.ps, r);
      uiCardEnd();
      r = uiCardBegin("##grade", tr(T_ColorGrading));
      uiColorGrading(p, a.er, a.ps, s, r);
      uiCardEnd();
      break;
    }
    case PG_DETAIL: {
      bool r = uiCardBegin("##def", tr(T_Presence));
      uiDefinition(p, a.er, r);
      uiCardEnd();
      r = uiCardBegin("##sharp", tr(T_Sharpening));
      uiSharpen(p, a.er, r);
      uiCardEnd();
      r = uiCardBegin("##noise", tr(T_NoiseReduction));
      uiNoise(p, a.er, r);
      uiCardEnd();
      break;
    }
    case PG_EFFECTS: {
      bool r = uiCardBegin("##vig", tr(T_Vignette));
      uiVignette(p, a.er, r);
      uiCardEnd();
      r = uiCardBegin("##grain", tr(T_Grain));
      uiGrain(p, a.er, r);
      uiCardEnd();
      break;
    }
    case PG_LOCAL: drawLocalPanel(a); break;
    case PG_LAYERS: drawLayersPanel(a); break;
    case PG_CROP:
      uiCardBegin("##crop", nullptr, false);
      drawCropPanel(a);
      uiCardEnd();
      break;
    case PG_LOOKS: drawLooksPage(a); break;
    case PG_HISTORY: drawHistoryPage(a); break;
  }
  ImGui::Dummy(ImVec2(0, 16.f * s));
  ImGui::EndDisabled();
  ImGui::EndChild();
}

// Two small triangles on the split handle: drag left / right.
static void arrowPair(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->AddTriangleFilled(ImVec2(c.x - r * 0.4f, c.y - r), ImVec2(c.x - r * 0.4f, c.y + r), ImVec2(c.x - r * 1.4f, c.y), col);
  dl->AddTriangleFilled(ImVec2(c.x + r * 0.4f, c.y - r), ImVec2(c.x + r * 1.4f, c.y), ImVec2(c.x + r * 0.4f, c.y + r), col);
}

static void drawCanvas(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 c0 = ImGui::GetCursorScreenPos();
  ImVec2 sz = ImGui::GetContentRegionAvail();
  if (sz.x < 8 || sz.y < 8) return;
  ImGui::InvisibleButton("##canvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(c0, ImVec2(c0.x + sz.x, c0.y + sz.y), pal::kCanvas);
  auto centerText = [&](const char* msg) {
    ImVec2 ts = ImGui::CalcTextSize(msg);
    ImVec2 pos(c0.x + (sz.x - ts.x) * 0.5f, c0.y + (sz.y - ts.y) * 0.5f);
    dl->AddRectFilled(ImVec2(pos.x - 16, pos.y - 10), ImVec2(pos.x + ts.x + 16, pos.y + ts.y + 10), IM_COL32(0, 0, 0, 150), 10);
    dl->AddText(pos, pal::kText, msg);
  };
  if (!a.hasImage) {
    if (a.load.active) {
      centerText(tr(T_Loading));
    } else {
      // Empty state: a drop zone that also opens the file dialog.
      float s = a.scale, bw = std::min(sz.x - 80.f * s, 460.f * s), bh = std::min(sz.y - 80.f * s, 260.f * s);
      ImVec2 b0(c0.x + (sz.x - bw) * 0.5f, c0.y + (sz.y - bh) * 0.5f), b1(b0.x + bw, b0.y + bh);
      ImU32 col = hovered ? pal::kText : pal::kFaint;
      float L = 26.f * s;
      for (int i = 0; i < 4; i++) {
        float x = (i & 1) ? b1.x : b0.x, y = (i & 2) ? b1.y : b0.y;
        float sx = (i & 1) ? -1.f : 1.f, sy = (i & 2) ? -1.f : 1.f;
        dl->AddLine(ImVec2(x, y), ImVec2(x + sx * L, y), col, 1.5f);
        dl->AddLine(ImVec2(x, y), ImVec2(x, y + sy * L), col, 1.5f);
      }
      ImVec2 cc((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f);
      dl->AddLine(ImVec2(cc.x - 8.f * s, cc.y - 30.f * s), ImVec2(cc.x + 8.f * s, cc.y - 30.f * s), col, 1.f);
      dl->AddLine(ImVec2(cc.x, cc.y - 38.f * s), ImVec2(cc.x, cc.y - 22.f * s), col, 1.f);
      ImGui::PushFont(g_fontTitle);
      const char* msg = tr(T_NoImage);
      ImVec2 ts = ImGui::CalcTextSize(msg);
      dl->AddText(ImVec2(cc.x - ts.x * 0.5f, cc.y - 4.f * s), pal::kText, msg);
      ImGui::PopFont();
      ImGui::PushFont(g_fontMono);
      const char* fmts = "JPEG  \xc2\xb7  PNG  \xc2\xb7  TIFF  \xc2\xb7  BMP";
      ImVec2 fs = ImGui::CalcTextSize(fmts);
      dl->AddText(ImVec2(cc.x - fs.x * 0.5f, cc.y + ts.y + 10.f * s), pal::kFaint, fmts);
      ImGui::PopFont();
      // Pointer to the other half of the app, for people who arrive with a whole folder.
      const char* hint = tr(T_EditToCull);
      ImGui::PushFont(g_fontBold);
      ImVec2 hs = ImGui::CalcTextSize(hint);
      float hw = hs.x + 24.f * s + 22.f * s, hh = hs.y + 14.f * s;
      ImVec2 h0(cc.x - hw * 0.5f, b1.y + 22.f * s), h1(h0.x + hw, h0.y + hh);
      bool hHov = hovered && io.MousePos.x >= h0.x && io.MousePos.x <= h1.x && io.MousePos.y >= h0.y && io.MousePos.y <= h1.y;
      dl->AddRectFilled(h0, h1, hHov ? IM_COL32(226, 82, 54, 70) : pal::kAccentSoft, 3.f * s);
      dl->AddRect(h0, h1, pal::kAccent, 3.f * s, 0, 1.5f);
      drawIcon(dl, IC_AI, ImVec2(h0.x + 16.f * s, (h0.y + h1.y) * 0.5f), 14.f * s, pal::kAccent);
      dl->AddText(ImVec2(h0.x + 30.f * s, h0.y + 7.f * s), pal::kAccent, hint);
      ImGui::PopFont();
      if (hHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      else if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (hovered && ImGui::IsMouseReleased(0)) {
        if (hHov) a.mode = 1;
        else doOpenDialog(a);
      }
    }
    return;
  }

  bool crop = a.cropMode;
  const EditParams& shown = a.hoverActive ? a.hoverParams : a.params;
  Geometry g = computeGeometry(shown, a.R.srcW(), a.R.srcH(), a.R.orientation(), crop);
  float outW = (float)g.outW, outH = (float)g.outH;
  float margin = (crop ? 40.f : 24.f) * a.scale;
  float bottomBar = crop ? 0.f : 76.f * a.scale;  // keep the photo clear of the floating toolbar
  float fitZ = std::max(0.0005f, std::min((sz.x - 2 * margin) / outW, (sz.y - 2 * margin - bottomBar) / outH));
  float z = a.view.fit ? fitZ : a.view.zoom;
  float dW = outW * z, dH = outH * z;
  float cyOff = a.view.fit ? -bottomBar * 0.5f : 0.f;

  auto zoomAt = [&](float nz, ImVec2 m) {
    ImVec2 tl(c0.x + sz.x * 0.5f - a.view.cx * dW, c0.y + sz.y * 0.5f + cyOff - a.view.cy * dH);
    float px = (m.x - tl.x) / dW, py = (m.y - tl.y) / dH;
    float nW = outW * nz, nH = outH * nz;
    a.view.cx = (c0.x + sz.x * 0.5f - (m.x - px * nW)) / nW;
    a.view.cy = (c0.y + sz.y * 0.5f - (m.y - py * nH)) / nH;
    a.view.fit = false;
    a.view.zoom = nz;
    z = nz;
    dW = nW;
    dH = nH;
    cyOff = 0.f;
  };
  if (hovered && io.MouseWheel != 0.f)
    zoomAt(std::clamp(z * std::pow(1.2f, io.MouseWheel), std::min(fitZ, 1.f) * 0.25f, 16.f), io.MousePos);
  bool layerMode = a.page == PG_LAYERS && !crop;
  bool layerOwns = layerMode && (a.layerDrag != 0 || a.layerTool == 3 || (a.layerTool != 0 && a.selLayer >= 0));
  if (!crop && !layerMode && hovered && ImGui::IsMouseDoubleClicked(0)) {
    if (a.view.fit) zoomAt(1.f, io.MousePos);
    else a.view.fit = true;
  }
  bool panning = active && !a.splitDrag &&
                 ((!crop && !layerOwns && ImGui::IsMouseDragging(0)) || ImGui::IsMouseDragging(1));
  if (panning) {
    a.view.cx -= io.MouseDelta.x / dW;
    a.view.cy -= io.MouseDelta.y / dH;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  if (a.view.fit) {
    z = fitZ;
    dW = outW * z;
    dH = outH * z;
    cyOff = -bottomBar * 0.5f;
  }
  if (dW <= sz.x) a.view.cx = 0.5f;
  else a.view.cx = std::clamp(a.view.cx, sz.x * 0.5f / dW, 1.f - sz.x * 0.5f / dW);
  if (dH <= sz.y) a.view.cy = 0.5f;
  else a.view.cy = std::clamp(a.view.cy, sz.y * 0.5f / dH, 1.f - sz.y * 0.5f / dH);
  ImVec2 tl(std::floor(c0.x + sz.x * 0.5f - a.view.cx * dW), std::floor(c0.y + sz.y * 0.5f + cyOff - a.view.cy * dH));
  a.viewZoom = z;

  float vx0 = std::floor(std::max(tl.x, c0.x)), vy0 = std::floor(std::max(tl.y, c0.y));
  float vx1 = std::floor(std::min(tl.x + dW, c0.x + sz.x)), vy1 = std::floor(std::min(tl.y + dH, c0.y + sz.y));
  int rw = (int)(vx1 - vx0), rh = (int)(vy1 - vy0);
  if (rw <= 0 || rh <= 0) return;
  float o0x = (vx0 - tl.x) / dW, o0y = (vy0 - tl.y) / dH, o1x = (vx1 - tl.x) / dW, o1y = (vy1 - tl.y) / dH;
  bool needBefore = !crop && (a.showBefore || a.split);

  RenderKey k;
  std::memset((void*)&k, 0, sizeof(k));
  k.p = shown;
  k.o[0] = o0x; k.o[1] = o0y; k.o[2] = o1x; k.o[3] = o1y;
  k.w = rw; k.h = rh; k.z = z;
  k.crop = crop; k.clip = a.clipWarn && !crop; k.before = needBefore;
  k.maskShow = a.hoverLayer; k.maskVer = a.maskVer;
  a.R.setMaskShow(a.hoverLayer);
  if (!a.keyValid || std::memcmp(&k, &a.lastKey, sizeof(k)) != 0) {
    a.texAfter = a.R.renderView(0, shown, crop, o0x, o0y, o1x, o1y, rw, rh, z, k.clip != 0, a.uA, a.vA);
    if (needBefore) {
      EditParams b;
      copyGeometry(b, shown);
      a.texBefore = a.R.renderView(1, b, false, o0x, o0y, o1x, o1y, rw, rh, z, false, a.uB, a.vB);
    }
    a.lastKey = k;
    a.keyValid = true;
  }

  // Viewfinder corner marks and a caption set like the edge print on a negative.
  if (!crop && dW < sz.x && dH < sz.y) {
    float o = 8.f * a.scale, L = 14.f * a.scale;
    ImU32 mc = pal::kFaint;
    for (int i = 0; i < 4; i++) {
      float x = (i & 1) ? vx1 + o : vx0 - o, y = (i & 2) ? vy1 + o : vy0 - o;
      float sx = (i & 1) ? -1.f : 1.f, sy = (i & 2) ? -1.f : 1.f;
      dl->AddLine(ImVec2(x, y), ImVec2(x + sx * L, y), mc, 1.f);
      dl->AddLine(ImVec2(x, y), ImVec2(x, y + sy * L), mc, 1.f);
    }
    ImGui::PushFont(g_fontMono);
    std::string name = fileName(a.path);
    for (char& ch : name) ch = (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
    char right[64];
    snprintf(right, sizeof(right), "%d \xc3\x97 %d   %d%%", g.outW, g.outH, (int)std::lround(z * 100.f));
    float ty = vy1 + o + 6.f * a.scale;
    if (ty + ImGui::GetTextLineHeight() < c0.y + sz.y - 52.f * a.scale) {
      dl->PushClipRect(ImVec2(vx0, ty), ImVec2(vx1 - ImGui::CalcTextSize(right).x - 20.f * a.scale, ty + 40), true);
      dl->AddText(ImVec2(vx0, ty), pal::kMuted, name.c_str());
      dl->PopClipRect();
      dl->AddText(ImVec2(vx1 - ImGui::CalcTextSize(right).x, ty), pal::kFaint, right);
    }
    ImGui::PopFont();
  }

  ImTextureID ta = (ImTextureID)(intptr_t)a.texAfter, tb = (ImTextureID)(intptr_t)a.texBefore;
  auto label = [&](const char* t, float x, float y, bool right) {
    ImVec2 ts = ImGui::CalcTextSize(t);
    if (right) x -= ts.x;
    dl->AddRectFilled(ImVec2(x - 8, y - 3), ImVec2(x + ts.x + 8, y + ts.y + 3), pal::kText, 2.f);
    dl->AddText(ImVec2(x, y), pal::kInk, t);
  };
  if (needBefore && a.split) {
    float sx = std::clamp(std::floor(c0.x + sz.x * a.splitPos), vx0, vx1);
    if (ImGui::IsItemActivated() && std::fabs(io.MousePos.x - sx) < 10.f * a.scale) a.splitDrag = true;
    if (a.splitDrag && active) {
      a.splitPos = std::clamp((io.MousePos.x - c0.x) / sz.x, 0.02f, 0.98f);
      sx = std::clamp(std::floor(c0.x + sz.x * a.splitPos), vx0, vx1);
    }
    if (!active) a.splitDrag = false;
    if (hovered && std::fabs(io.MousePos.x - sx) < 10.f * a.scale) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    float us = (sx - vx0) / (vx1 - vx0);
    dl->AddImage(tb, ImVec2(vx0, vy0), ImVec2(sx, vy1), ImVec2(0, 0), ImVec2(us * a.uB, a.vB));
    dl->AddImage(ta, ImVec2(sx, vy0), ImVec2(vx1, vy1), ImVec2(us * a.uA, 0), ImVec2(a.uA, a.vA));
    dl->AddLine(ImVec2(sx, vy0), ImVec2(sx, vy1), pal::kText, 1.f);
    ImVec2 hc(sx, (vy0 + vy1) * 0.5f);
    float hw = 9.f * a.scale, hh = 16.f * a.scale;
    dl->AddRectFilled(ImVec2(hc.x - hw, hc.y - hh), ImVec2(hc.x + hw, hc.y + hh), pal::kText, 2.f);
    arrowPair(dl, hc, 3.5f * a.scale, pal::kInk);
    float ly = vy1 - ImGui::GetTextLineHeight() - 14;  // bottom corners: the histogram sits top-right
    label(tr(T_Before), vx0 + 14, ly, false);
    label(tr(T_After), vx1 - 14, ly, true);
  } else if (needBefore) {
    dl->AddImage(tb, ImVec2(vx0, vy0), ImVec2(vx1, vy1), ImVec2(0, 0), ImVec2(a.uB, a.vB));
    label(tr(T_Before), vx0 + 14, vy1 - ImGui::GetTextLineHeight() - 14, false);
  } else {
    dl->AddImage(ta, ImVec2(vx0, vy0), ImVec2(vx1, vy1), ImVec2(0, 0), ImVec2(a.uA, a.vA));
  }
  if (layerMode && !needBefore) layerCanvas(a, g, tl, dW, dH, hovered, active);
  if (crop) cropOverlay(a, tl, dW, dH, active);
  if (a.load.active) centerText(tr(T_Loading));
}

// Controls over the photo: a flat strip of text toggles (no pill, no icons) and the histogram.
static void drawCanvasOverlays(App& a, ImVec2 c0, ImVec2 sz) {
  if (!a.hasImage) return;
  float s = a.scale;
  if (a.showHisto && !a.cropMode) {
    float hw = std::min(232.f * s, sz.x * 0.4f), hh = 84.f * s;
    ImGui::SetCursorScreenPos(ImVec2(c0.x + sz.x - hw - 16.f * s, c0.y + 16.f * s));
    uiHistogram(a.histo, s, hw, hh, true);
  }
  if (a.cropMode) return;
  if (a.page == PG_LAYERS) {  // tool palette down the left edge, like an image editor's toolbox
    float bs = 38.f * s, pad = 6.f * s;
    struct T { int icon, tool; const char* name; const char* key; };
    const T tools[4] = {{IC_MOVE, 0, tr(T_ToolMove), "V"}, {IC_WAND, 3, tr(T_ToolMagic), "J"},
                        {IC_ERASER, 1, tr(T_ToolErase), "E"}, {IC_BRUSH, 2, tr(T_ToolRestore), "B"}};
    float ph = pad * 2 + bs * 5 + 10.f * s;
    ImVec2 pp(c0.x + 14.f * s, c0.y + 14.f * s);
    ImGui::SetCursorScreenPos(pp);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(24, 23, 21, 240));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##composetools", ImVec2(bs + pad * 2, ph), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    pdl->AddRect(pp, ImVec2(pp.x + bs + pad * 2, pp.y + ph), pal::kFrame, 3.f * s);
    for (const T& t : tools) {
      bool needLayer = t.tool == 1 || t.tool == 2;
      char tip[160];
      snprintf(tip, sizeof(tip), "%s  (%s)%s%s", t.name, t.key, needLayer && a.selLayer < 0 ? " - " : "",
               needLayer && a.selLayer < 0 ? tr(T_NeedLayer) : "");
      ImGui::PushID(t.tool);
      ImGui::BeginDisabled(needLayer && a.selLayer < 0);
      if (iconButton("##tool", t.icon, a.layerTool == t.tool, tip, bs)) a.layerTool = t.tool;
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 10.f * s));
    if (iconButton("##addphoto", IC_PLUS, false, tr(T_AddPhotoLayer), bs))
      for (const std::string& f : openImagesDialog(glfwGetWin32Window(a.win), a.S.lastDir)) addImageLayer(a, f);
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
  }
  const char* lb[6] = {tr(T_TbOriginal), tr(T_TbSplit), tr(T_TbFit), "1:1", tr(T_TbClip), tr(T_TbHisto)};
  float pad = 8.f * s, sep = 18.f * s;
  float tw = pad * 2 + sep * 2;
  for (const char* l : lb) tw += ImGui::CalcTextSize(l).x + 14.f * s;
  float th = ImGui::GetFrameHeight() + 6.f * s;
  ImVec2 bp(std::floor(c0.x + (sz.x - tw) * 0.5f), std::floor(c0.y + sz.y - th - 12.f * s));
  ImGui::SetCursorScreenPos(bp);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(24, 23, 21, 235));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 3.f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::BeginChild("##viewbar", ImVec2(tw, th), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRect(bp, ImVec2(bp.x + tw, bp.y + th), pal::kFrame, 2.f * s);
  tourSetWindow(TR_VIEWBAR);
  auto separator = [&]() {
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fh = ImGui::GetFrameHeight();
    dl->AddLine(ImVec2(p.x + sep * 0.5f, p.y + fh * 0.3f), ImVec2(p.x + sep * 0.5f, p.y + fh * 0.7f), pal::kFrame, 1.f);
    ImGui::Dummy(ImVec2(sep, fh));
    ImGui::SameLine(0, 0);
  };
  if (uiTextToggle(lb[0], a.showBefore, tr(T_BeforeAfter))) {
    a.showBefore = !a.showBefore;
    if (a.showBefore) a.split = false;
  }
  ImGui::SameLine(0, 0);
  if (uiTextToggle(lb[1], a.split, tr(T_Split))) {
    a.split = !a.split;
    if (a.split) a.showBefore = false;
  }
  separator();
  if (uiTextToggle(lb[2], a.view.fit, tr(T_Fit))) a.view.fit = true;
  ImGui::SameLine(0, 0);
  if (uiTextToggle(lb[3], !a.view.fit && std::fabs(a.view.zoom - 1.f) < 1e-3f, tr(T_Zoom100))) {
    a.view.fit = false;
    a.view.zoom = 1.f;
  }
  separator();
  if (uiTextToggle(lb[4], a.clipWarn, tr(T_ClipWarn))) a.clipWarn = !a.clipWarn;
  ImGui::SameLine(0, 0);
  if (uiTextToggle(lb[5], a.showHisto, tr(T_ShowHisto))) a.showHisto = !a.showHisto;
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- tutorial (spotlight tour)
struct TourStep { int target, mode, page, title, body; };
static const TourStep kTour[] = {
    {-1, 0, -1, T_Tr0T, T_Tr0B},            {TR_OPEN, 0, -1, T_Tr1T, T_Tr1B},
    {TR_RAIL, 0, PG_LIGHT, T_Tr2T, T_Tr2B}, {TR_TOOLS, 0, PG_LIGHT, T_Tr3T, T_Tr3B},
    {TR_TOOLS, 0, PG_AI, T_Tr4T, T_Tr4B},   {TR_VIEWBAR, 0, PG_LIGHT, T_Tr5T, T_Tr5B},
    {TR_EXPORT, 0, -1, T_Tr6T, T_Tr6B},     {TR_CULLTAB, 0, -1, T_Tr7T, T_Tr7B},
    {TR_CFOLDER, 1, -1, T_Tr8T, T_Tr8B},    {TR_CLEFT, 1, -1, T_Tr9T, T_Tr9B},
    {TR_CGRID, 1, -1, T_Tr10T, T_Tr10B},    {-1, 0, -1, T_Tr11T, T_Tr11B},
};
static const int kTourSteps = (int)(sizeof(kTour) / sizeof(kTour[0]));

static void tourGo(App& a, int step) {
  a.tourStep = step;
  const TourStep& t = kTour[step];
  a.mode = t.mode;
  if (t.page >= 0) setPage(a, t.page);
}

static void tourEnd(App& a) {
  a.tourStep = -1;
  a.mode = 0;
  a.S.tutorialDone = 1;
  saveSettings(a.S);
}

static void drawTour(App& a) {
  if (a.tourStep < 0) return;
  float s = a.scale;
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  const TourStep& st = kTour[a.tourStep];
  bool last = a.tourStep == kTourSteps - 1;
  ImVec2 ta, tb;
  bool has = st.target >= 0 && tourGet(st.target, ta, tb);

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ds);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::Begin("##tour", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar(2);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 dim = IM_COL32(8, 7, 6, 196);
  ImVec2 ha(0, 0), hb(0, 0);
  if (has) {
    float pad = 6.f * s;
    ha = ImVec2(std::max(0.f, ta.x - pad), std::max(0.f, ta.y - pad));
    hb = ImVec2(std::min(ds.x, tb.x + pad), std::min(ds.y, tb.y + pad));
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(ds.x, ha.y), dim);
    dl->AddRectFilled(ImVec2(0, hb.y), ds, dim);
    dl->AddRectFilled(ImVec2(0, ha.y), ImVec2(ha.x, hb.y), dim);
    dl->AddRectFilled(ImVec2(hb.x, ha.y), ImVec2(ds.x, hb.y), dim);
    dl->AddRect(ha, hb, pal::kAccent, 3.f * s, 0, 2.f);
  } else {
    dl->AddRectFilled(ImVec2(0, 0), ds, dim);
  }
  ImGui::SetCursorScreenPos(ImVec2(0, 0));
  ImGui::SetNextItemAllowOverlap();  // else this button would claim the hover and block the card's own buttons
  ImGui::InvisibleButton("##tourblock", ds);  // modal: nothing underneath receives clicks

  // ---- the card
  float cw = std::min(380.f * s, ds.x - 40.f * s), pad = 20.f * s, gap = 16.f * s;
  const char* title = tr(st.title);
  const char* body = tr(st.body);
  ImGui::PushFont(g_fontTitle);
  float titleH = ImGui::CalcTextSize(title, nullptr, false, cw - pad * 2).y;
  ImGui::PopFont();
  float bodyH = ImGui::CalcTextSize(body, nullptr, false, cw - pad * 2).y;
  float fh = ImGui::GetFrameHeight();
  float top = pad * 0.8f, cntH = 14.f * s;
  float ch = top + cntH + 4.f * s + titleH + 8.f * s + bodyH + 18.f * s + fh + pad;
  ImVec2 pos((ds.x - cw) * 0.5f, (ds.y - ch) * 0.5f);
  if (has) {
    bool tall = (hb.y - ha.y) > ds.y * 0.5f;
    auto fits = [&](ImVec2 p) { return p.x >= 10 && p.y >= 10 && p.x + cw <= ds.x - 10 && p.y + ch <= ds.y - 10; };
    float cx = std::clamp((ha.x + hb.x) * 0.5f - cw * 0.5f, 10.f, ds.x - cw - 10.f);
    float cyy = std::clamp((ha.y + hb.y) * 0.5f - ch * 0.5f, 10.f, ds.y - ch - 10.f);
    ImVec2 below(cx, hb.y + gap), above(cx, ha.y - gap - ch), right(hb.x + gap, cyy), left(ha.x - gap - cw, cyy);
    ImVec2 order[4];
    if (tall) order[0] = right, order[1] = left, order[2] = below, order[3] = above;
    else order[0] = below, order[1] = above, order[2] = right, order[3] = left;
    bool placed = false;
    for (const ImVec2& p : order)
      if (fits(p)) {
        pos = p;
        placed = true;
        break;
      }
    if (!placed)  // nothing fits beside it: overlap the target's inner corner
      pos = ImVec2(std::clamp(hb.x - cw - gap, 10.f, ds.x - cw - 10.f), std::clamp(hb.y - ch - gap, 10.f, ds.y - ch - 10.f));
  }
  pos.x = std::floor(pos.x), pos.y = std::floor(pos.y);
  ImVec2 pe(pos.x + cw, pos.y + ch);
  dl->AddRectFilled(ImVec2(pos.x + 3, pos.y + 5), ImVec2(pe.x + 3, pe.y + 5), IM_COL32(0, 0, 0, 120), 4.f * s);
  dl->AddRectFilled(pos, pe, pal::kPanel, 3.f * s);
  dl->AddRect(pos, pe, pal::kTrack, 3.f * s);
  dl->AddRectFilled(pos, ImVec2(pe.x, pos.y + 3.f * s), pal::kAccent, 3.f * s, ImDrawFlags_RoundCornersTop);

  // step counter in the DIN face, then title and body
  char cnt[24];
  snprintf(cnt, sizeof(cnt), "%02d / %02d", a.tourStep + 1, kTourSteps);
  ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + top));
  ImGui::PushFont(g_fontMono);
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kAccent);
  ImGui::TextUnformatted(cnt);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  float ty = pos.y + top + cntH + 4.f * s;
  ImGui::PushTextWrapPos(pos.x + cw - pad);
  ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, ty));
  ImGui::PushFont(g_fontTitle);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, ty + titleH + 8.f * s));
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
  ImGui::TextUnformatted(body);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();

  // buttons
  float by = pe.y - pad - fh;
  bool next = false, back = false, skip = false;
  float bw = 120.f * s;
  ImGui::SetCursorScreenPos(ImVec2(pe.x - pad - bw, by));
  if (uiPrimaryButton(last ? trId(T_TourDone, "tdone") : trId(T_TourNext, "tnext"), ImVec2(bw, fh))) next = true;
  if (a.tourStep > 0) {
    ImGui::SetCursorScreenPos(ImVec2(pe.x - pad - bw - 8.f * s - 88.f * s, by));
    if (ImGui::Button(trId(T_TourBack, "tback"), ImVec2(88.f * s, fh))) back = true;
  }
  if (!last) {
    ImGui::SetCursorScreenPos(ImVec2(pos.x + pad - 6.f * s, by));
    if (ImGui::Button(trId(T_TourSkip, "tskip"), ImVec2(64.f * s, fh))) skip = true;
  }
  ImGui::End();

  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) skip = true;
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Space, false))
    next = true;
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && a.tourStep > 0) back = true;
  if (skip) tourEnd(a);
  else if (next) last ? tourEnd(a) : tourGo(a, a.tourStep + 1);
  else if (back) tourGo(a, a.tourStep - 1);
}

static void drawStatusBar(App& a) {
  ImGui::SetCursorPos(ImVec2(14 * a.scale, ImGui::GetCursorPosY() + 3 * a.scale));
  ImGui::PushFont(g_fontMono);
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kFaint);
  if (a.hasImage && a.mode == 0) {
    ImGui::Text("%s", a.path.c_str());
  } else {
    ImGui::TextUnformatted("Circle Room");
  }
  char right[256];
  snprintf(right, sizeof(right), "RAM %.0f MB   /   %s", processPrivateWorkingSet() / (1024.0 * 1024.0), a.gpuName.c_str());
  float tw = ImGui::CalcTextSize(right).x;
  ImGui::SameLine(ImGui::GetWindowWidth() - tw - 14 * a.scale);
  ImGui::TextUnformatted(right);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

static void drawPopups(App& a) {
  float s = a.scale;
  if (a.openExport) {
    ImGui::OpenPopup("###exportdlg");
    a.openExport = false;
  }
  ImGui::SetNextWindowSize(ImVec2(380 * s, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * s, 16 * s));
  if (ImGui::BeginPopupModal(trId(T_ExportTitle, "exportdlg"), nullptr, ImGuiWindowFlags_NoResize)) {
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
    ImGui::TextUnformatted(tr(T_Format));
    ImGui::PopStyleColor();
    const char* fmts[2] = {"JPEG", "PNG"};
    int f = uiSegmented("fmt", fmts, 2, a.S.expPng);
    if (f >= 0) a.S.expPng = f;
    ImGui::Dummy(ImVec2(0, 4 * s));
    if (!a.S.expPng) {
      float q = (float)a.S.expQuality;
      EditResult tmp;
      uiSlider(tmp, T_Quality, "q", &q, 10, 100, 92, "%.0f");
      a.S.expQuality = (int)q;
    }
    bool rs = a.S.expResize != 0;
    if (ImGui::Checkbox(trId(T_ResizeLong, "resize"), &rs)) a.S.expResize = rs;
    if (rs) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputInt("##long", &a.S.expLong, 100, 500);
      a.S.expLong = std::clamp(a.S.expLong, 64, 65535);
    }
    int w, h;
    outputDims(a, w, h);
    uiHint((std::string(tr(T_OutputSize)) + ": " + std::to_string(w) + " × " + std::to_string(h) + " px").c_str());
    ImGui::Dummy(ImVec2(0, 6 * s));
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(trId(T_Cancel, "cancel"), ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (uiPrimaryButton(trId(T_Export, "doexport"), ImVec2(bw, 0))) {
      std::string def = dirOf(a.path) + "\\" + fileStem(a.path) + "_edit" + (a.S.expPng ? ".png" : ".jpg");
      std::string out = saveImageDialog(glfwGetWin32Window(a.win), def, a.S.expPng != 0);
      if (!out.empty()) startExport(a, out);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (a.openSavePreset) {
    ImGui::OpenPopup("###savepreset");
    a.openSavePreset = false;
  }
  ImGui::SetNextWindowSize(ImVec2(340 * s, 0));
  if (ImGui::BeginPopupModal(trId(T_SavePreset, "savepreset"), nullptr, ImGuiWindowFlags_NoResize)) {
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
    ImGui::TextUnformatted(tr(T_PresetName));
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##pname", a.presetName, sizeof(a.presetName), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Dummy(ImVec2(0, 4 * s));
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(trId(T_Cancel, "pcancel"), ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if ((uiPrimaryButton(trId(T_Save, "psave"), ImVec2(bw, 0)) || enter) && a.presetName[0]) {
      std::string name = a.presetName;
      for (char& ch : name)
        if (strchr("\\/:*?\"<>|", ch)) ch = '_';
      ensureDir(presetsDir());
      writeTextFile(presetsDir() + "\\" + name + kLookExt, serializeParams(a.params, false));
      loadPresets(a);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (a.openNewStyle) {
    ImGui::OpenPopup("###newstyle");
    a.openNewStyle = false;
  }
  if (ImGui::BeginPopupModal(trId(T_NewStyle, "newstyle"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
    ImGui::TextUnformatted(tr(T_StyleName));
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(300 * s);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##nsname", a.newStyleName, sizeof(a.newStyleName));
    uiHint(tr(T_NeedRefs));
    ImGui::Dummy(ImVec2(0, 4 * s));
    if (ImGui::Button(trId(T_Cancel, "nscancel"), ImVec2(146 * s, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (uiPrimaryButton(trId(T_ChoosePhotos, "nschoose"), ImVec2(146 * s, 0)) && a.newStyleName[0]) {
      std::vector<std::string> files = openImagesDialog(glfwGetWin32Window(a.win), a.S.lastDir);
      if (files.size() >= 5) a.styles.startCreate(a.newStyleName, files);
      else if (!files.empty()) showToast(a, tr(T_NeedRefs));
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();

  if (!a.toast.empty() && Clock::now() < a.toastUntil) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImVec2 ts = ImGui::CalcTextSize(a.toast.c_str());
    ImVec2 pos((ds.x - ts.x) * 0.5f, 70 * s);
    fg->AddRectFilled(ImVec2(pos.x - 18, pos.y - 10), ImVec2(pos.x + ts.x + 16, pos.y + ts.y + 10), pal::kText, 2.f);
    fg->AddRectFilled(ImVec2(pos.x - 18, pos.y - 10), ImVec2(pos.x - 14, pos.y + ts.y + 10), pal::kAccent);
    fg->AddText(ImVec2(pos.x, pos.y), pal::kInk, a.toast.c_str());
  }
}

static void handleShortcuts(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || a.tourStep >= 0) return;
  if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) tourGo(a, 0);
  if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))) stepTextSize(a, +1);
  if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))) stepTextSize(a, -1);
  if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false))) requestTextSize(a, 100);
  bool ctrl = io.KeyCtrl;
  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) doOpenDialog(a);
  if (!a.hasImage || a.mode != 0) return;
  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_E, false) && a.exp.stage == 0) a.openExport = true;
  if (ctrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) doUndo(a);
  if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) doRedo(a);
  if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Backslash, false)) {
    a.showBefore = !a.showBefore;
    a.split = false;
  }
  if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
    if (a.cropMode) {
      a.cropMode = false;
    } else if (a.page != PG_CROP) {
      setPage(a, PG_CROP);
    } else {
      a.cropMode = true;
      a.view.fit = true;
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && a.cropMode) a.cropMode = false;
  if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
    a.view.fit = !a.view.fit;
    if (!a.view.fit) a.view.zoom = 1.f;
  }
  // 1-9 jump to a tool page
  for (int i = 0; i < std::min(9, (int)PG_COUNT) && !ctrl; i++)
    if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false)) setPage(a, i);
  if (!ctrl && PG_COUNT > 9 && ImGui::IsKeyPressed(ImGuiKey_0, false)) setPage(a, 9);
  if (a.page == PG_LAYERS && !ctrl && !a.cropMode) {  // Photoshop-like tool keys
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) a.layerTool = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_J, false)) a.layerTool = 3;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && a.selLayer >= 0) a.layerTool = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_B, false) && a.selLayer >= 0) a.layerTool = 2;
  }
  if (a.page == PG_LAYERS && a.layerTool != 0) {  // brush size
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) a.brushPx = std::max(4.f, a.brushPx / 1.15f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) a.brushPx = std::min(800.f, a.brushPx * 1.15f);
  }
}

static void buildUI(App& a) {
  a.er = EditResult();
  tourClear();
  if (!a.tourAutoTried) {  // first launch: start the tutorial once
    a.tourAutoTried = true;
    if (!a.S.tutorialDone) tourGo(a, 0);
  }
  handleShortcuts(a);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar(3);
  float s = a.scale;
  float barH = 52 * s, statusH = 24 * s;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal::kRail);
  ImGui::BeginChild("##top", ImVec2(0, barH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 6 * s));
  drawTopBar(a, barH);
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  {  // hairline under the top bar
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y), pal::kFrame, 1.f);
  }

  float railW = 136 * s, panelW = 340 * s;
  float bodyH = ImGui::GetContentRegionAvail().y - statusH;
  if (a.mode == 1) {
    CullHost host = cullHost(a);
    ImGui::BeginChild("##cullbody", ImVec2(0, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 10 * s));
    a.cull.draw(host, a.styles);
    ImGui::PopStyleVar();
    ImGui::EndChild();
  } else {
    updateHistogram(a);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal::kRail);
    ImGui::BeginChild("##rail", ImVec2(railW, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    drawRail(a);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * s, 12 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 6 * s));
    ImGui::BeginChild("##tools", ImVec2(panelW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    drawToolPanel(a);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##canvaswin", ImVec2(0, bodyH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 c0 = ImGui::GetCursorScreenPos(), csz = ImGui::GetContentRegionAvail();
    drawCanvas(a);
    drawCanvasOverlays(a, c0, csz);
    ImGui::EndChild();
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal::kRail);
  ImGui::BeginChild("##status", ImVec2(0, statusH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  drawStatusBar(a);
  ImGui::EndChild();
  ImGui::PopStyleColor();
  drawPopups(a);
  ImGui::End();
  drawTour(a);

  if (a.er.commit >= 0 && a.hasImage) {
    a.hist.push(a.params, a.er.commit, a.er.extra);
    saveSidecar(a);
  }
}

// ---------------------------------------------------------------- setup
static GLFWwindow* createWindow(int w, int h, bool visible, std::string& err) {
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
  glfwWindowHint(GLFW_DEPTH_BITS, 0);
  glfwWindowHint(GLFW_STENCIL_BITS, 0);
  GLFWwindow* win = glfwCreateWindow(w, h, "Circle Room", nullptr, nullptr);
  if (!win) {
    err = "OpenGL 3.3 is not available";
    return nullptr;
  }
  glfwMakeContextCurrent(win);
  const char* missing = glpLoad([](const char* n) { return (void*)glfwGetProcAddress(n); });
  if (missing) {
    err = std::string("missing GL function ") + missing;
    glfwDestroyWindow(win);
    return nullptr;
  }
  return win;
}

static ImFont* addUiFont(const std::string& dir, const char* latin, const char* thai, float size) {
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  ImFont* f = nullptr;
  if (fileExists(dir + latin)) f = io.Fonts->AddFontFromFileTTF((dir + latin).c_str(), size, &cfg, io.Fonts->GetGlyphRangesDefault());
  if (!f) return nullptr;
  // Thai glyphs merged into the same font; only the Thai range (+ a few symbols) is rasterised.
  std::string th = fileExists(dir + thai) ? dir + thai : dir + "tahoma.ttf";
  if (fileExists(th)) {
    ImFontConfig m;
    m.MergeMode = true;
    m.OversampleH = 2;
    m.OversampleV = 1;
    io.Fonts->AddFontFromFileTTF(th.c_str(), size * 1.05f, &m, io.Fonts->GetGlyphRangesThai());
  }
  static const ImWchar extra[] = {0x00B0, 0x00B7, 0x00D7, 0x00D7, 0x2026, 0x2026, 0};  // ° · × …
  ImFontConfig e;
  e.MergeMode = true;
  io.Fonts->AddFontFromFileTTF((dir + latin).c_str(), size, &e, extra);
  return f;
}

static void setupFonts(float scale) {
  ImGuiIO& io = ImGui::GetIO();
  const char* windir = getenv("WINDIR");
  std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
  float size = std::round(15.f * scale);
  ImFont* regular = addUiFont(fonts, "segoeui.ttf", "LeelawUI.ttf", size);
  if (!regular) regular = io.Fonts->AddFontDefault();
  // Bahnschrift is Windows' DIN face: headings and numbers read like camera markings.
  g_fontBold = addUiFont(fonts, "bahnschrift.ttf", "LeelaUIb.ttf", std::round(16.f * scale));
  if (!g_fontBold) g_fontBold = addUiFont(fonts, "seguisb.ttf", "LeelaUIb.ttf", size);
  if (!g_fontBold) g_fontBold = regular;
  g_fontTitle = addUiFont(fonts, "bahnschrift.ttf", "LeelaUIb.ttf", std::round(21.f * scale));
  if (!g_fontTitle) g_fontTitle = g_fontBold;
  g_fontSmall = addUiFont(fonts, "segoeui.ttf", "LeelawUI.ttf", std::round(12.5f * scale));
  if (!g_fontSmall) g_fontSmall = regular;
  g_fontMono = addUiFont(fonts, "bahnschrift.ttf", "LeelawUI.ttf", std::round(13.f * scale));
  if (!g_fontMono) g_fontMono = g_fontSmall;
}

static bool endsWithI(const std::string& s, const char* suf) {
  size_t n = strlen(suf);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}

// CircleRoom.exe --export in.jpg params.pedit|- out.jpg [--long N] [--quality Q]
// CircleRoom.exe --bench in.jpg
static int runHeadless(const std::vector<std::string>& args) {
  attachParentConsole();
  bool bench = args[1] == "--bench";
  if ((!bench && args.size() < 5) || (bench && args.size() < 3)) {
    fprintf(stderr, "usage: --export in params|- out [--long N] [--quality Q] | --bench in\n");
    return 2;
  }
  int longEdge = 0, quality = 92;
  for (size_t i = bench ? 3 : 5; i + 1 < args.size(); i += 2) {
    if (args[i] == "--long") longEdge = atoi(args[i + 1].c_str());
    if (args[i] == "--quality") quality = atoi(args[i + 1].c_str());
  }
  if (!glfwInit()) return 3;
  std::string err;
  GLFWwindow* win = createWindow(64, 64, false, err);
  if (!win) {
    fprintf(stderr, "%s\n", err.c_str());
    return 3;
  }
  Renderer R;
  if (!R.init(err)) {
    fprintf(stderr, "shader error: %s\n", err.c_str());
    return 4;
  }
  auto mem = [](const char* stage) {
    printf("  [mem] %-14s commit %6.1f MB  working set %6.1f MB  private WS %6.1f MB\n", stage,
           processPrivateBytes() / 1048576.0, processWorkingSet() / 1048576.0, processPrivateWorkingSet() / 1048576.0);
  };
  mem("gl init");
  auto t0 = Clock::now();
  ImageData img;
  if (!loadImage(args[2], R.maxTextureSize(), img, err)) {
    fprintf(stderr, "load failed: %s\n", err.c_str());
    return 5;
  }
  mem("decoded");
  if (!R.upload(img, err)) {
    fprintf(stderr, "upload failed: %s\n", err.c_str());
    return 5;
  }
  mem("uploaded");
  trimWorkingSet();
  mem("trimmed");
  auto ms = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
  printf("GPU: %s\nloaded %dx%d (orientation %d) in %.0f ms, RAM %.1f MB\n", (const char*)glGetString(GL_RENDERER),
         R.srcW(), R.srcH(), R.orientation(), ms(t0), processPrivateBytes() / 1048576.0);
  EditParams p;
  if (bench) {
    deserializeParams("exposure=0.4\ncontrast=25\nhighlights=-40\nshadows=35\nclarity=20\ntexture=15\ndehaze=10\n"
                      "vibrance=20\ncurve0=0,0;0.3,0.25;0.7,0.78;1,1\nhslSat2=20\ngrade0Sat=20\nsharpAmount=60\n"
                      "nrLum=20\nvigAmount=-20\ngrainAmount=10\n", p, true);
    float u, v;
    for (int i = 0; i < 5; i++) R.renderView(0, p, false, 0, 0, 1, 1, 1600, 1000, 0.3f, false, u, v);
    glFinish();
    auto tb = Clock::now();
    const int N = 60;
    for (int i = 0; i < N; i++) {
      p.exposure = 0.4f + i * 0.001f;
      R.renderView(0, p, false, 0, 0, 1, 1, 1600, 1000, 0.3f, false, u, v);
    }
    glFinish();
    printf("preview 1600x1000, all adjustments: %.2f ms/frame\n", ms(tb) / N);
    unsigned h[4][256];
    tb = Clock::now();
    for (int i = 0; i < 20; i++) R.histogram(p, h);
    printf("histogram: %.2f ms\nRAM %.1f MB\n", ms(tb) / 20, processPrivateBytes() / 1048576.0);
  } else {
    std::string t;
    if (args[3] != "-") {
      if (!readTextFile(args[3], t)) {
        fprintf(stderr, "cannot read params %s\n", args[3].c_str());
        return 6;
      }
      deserializeParams(t, p, true);
    }
    if (p.recover > 0) {
      ai::Recovery rec;
      auto tr0 = Clock::now();
      if (ai::recoverFile(args[2], nullptr, true, rec, err) && rec.valid()) R.setRecover(rec.rgba.data(), rec.w, rec.h);
      printf("highlight recovery: %.0f ms\n", ms(tr0));
    }
    if (anyLayer(p)) {
      ai::SegResult seg;
      std::vector<float> guide;
      std::vector<uint8_t> mask;
      int mw = 0, mh = 0;
      auto ts = Clock::now();
      if (!ai::segmentFile(args[2], seg, guide, mw, mh, err)) {
        fprintf(stderr, "segmentation failed: %s\n", err.c_str());
        return 8;
      }
      ai::buildMasks(seg, guide, mw, mh, p.layers, mask);
      R.setMask(mask.data(), mw, mh);
      printf("regions + masks: %.0f ms\n", ms(ts));
    }
    HealStore healStore;
    if (p.heals > 0) {
      auto th0 = Clock::now();
      healStore.parse(t);
      healStore.sync(p, R, args[2], true);
      printf("magic eraser: %.0f ms\n", ms(th0));
    }
    LayerStore layerStore;
    if (anyImageLayer(p)) {
      auto tl0 = Clock::now();
      layerStore.parseStrokes(t);
      layerStore.sync(p, R, true);
      layerStore.sync(p, R, false);  // masks need the finished segmentation
      printf("photo layers: %.0f ms\n", ms(tl0));
    }
    Geometry g = computeGeometry(p, R.srcW(), R.srcH(), R.orientation(), false);
    int w = g.outW, h = g.outH;
    if (longEdge > 0 && longEdge < std::max(w, h)) {
      double s = (double)longEdge / std::max(w, h);
      w = std::max(1, (int)std::lround(w * s));
      h = std::max(1, (int)std::lround(h * s));
    }
    auto te = Clock::now();
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    R.exportBegin(p, w, h);
    while (!R.exportStep(rgb.data())) {}
    double renderMs = ms(te);
    size_t peak = processPrivateBytes();
    if (!writeImage(args[4], rgb.data(), w, h, endsWithI(args[4], ".png"), quality, err)) {
      fprintf(stderr, "write failed: %s\n", err.c_str());
      return 7;
    }
    printf("exported %dx%d: render %.0f ms, total %.0f ms, RAM during export %.1f MB\n", w, h, renderMs, ms(te),
           peak / 1048576.0);
  }
  R.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}

// CircleRoom.exe --ai-tokcheck <tests.txt>: compares C++ tokenizers with reference ids (tab separated)
static int runTokCheck(const std::vector<std::string>& args) {
  attachParentConsole();
  ai::UnigramTokenizer mt;
  ai::ClipBpe bpe;
  if (args.size() < 3 || !mt.load(ai::aiDir() + "\\mt_vocab.bin") || !bpe.load(ai::aiDir() + "\\clip_bpe.bin")) return 2;
  std::string text;
  readTextFile(args[2], text);
  int bad = 0, total = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    pos = e == std::string::npos ? text.size() : e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos) continue;
    std::string kind = line.substr(0, t1), s = line.substr(t1 + 1, t2 - t1 - 1), want = line.substr(t2 + 1);
    std::vector<int64_t> ids = kind == "mt" ? mt.encode(s) : bpe.encode(s);
    std::string got;
    for (size_t i = 0; i < ids.size(); i++) {
      if (kind == "clip" && i > 0 && ids[i] == 49407 && ids[i - 1] == 49407) break;  // padding
      got += (got.empty() ? "" : " ") + std::to_string(ids[i]);
    }
    total++;
    if (got != want) {
      bad++;
      printf("MISMATCH %s\n  want %s\n  got  %s\n", kind.c_str(), want.c_str(), got.c_str());
    }
  }
  printf("%d/%d tokenizer cases match\n", total - bad, total);
  return bad ? 1 : 0;
}

// CircleRoom.exe --ai-search <folder> "<query>" [--top N]
static int runAiSearch(const std::vector<std::string>& args) {
  attachParentConsole();
  if (args.size() < 4) return 2;
  int top = 5;
  for (size_t i = 4; i + 1 < args.size(); i++)
    if (args[i] == "--top") top = atoi(args[i + 1].c_str());
  ai::Analyzer an;  // makes sure the folder has embeddings (uses the cache)
  an.startFolder(args[2], false);
  while (an.running()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  an.poll();
  std::vector<ai::PhotoInfo> photos = an.takeResults();
  auto t0 = Clock::now();
  ai::TextQuery q;
  std::string err, english;
  if (!q.load(err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 5;
  }
  double loadMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  size_t ram = processPrivateWorkingSet();
  t0 = Clock::now();
  float v[ai::kEmbDim];
  if (!q.embed(args[3], v, &english, err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 5;
  }
  double qMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  std::vector<std::pair<float, int>> s;
  for (size_t i = 0; i < photos.size(); i++) {
    float d = 0;
    for (int k = 0; k < ai::kEmbDim; k++) d += v[k] * photos[i].emb[k];
    s.push_back({d, (int)i});
  }
  std::sort(s.rbegin(), s.rend());
  printf("query \"%s\" -> \"%s\"  (load %.0f ms, query %.0f ms, RAM with models %.0f MB)\n", args[3].c_str(),
         english.c_str(), loadMs, qMs, ram / 1048576.0);
  for (int i = 0; i < top && i < (int)s.size(); i++)
    printf("  %d. %-28s %.3f\n", i + 1, fileName(photos[s[i].second].path).c_str(), s[i].first);
  q.release();
  ai::ortUnload();
  return 0;
}

// CircleRoom.exe --ai-recover <in> <out.jpg> [--strength S] [--no-regions]
static int runAiRecover(const std::vector<std::string>& args) {
  attachParentConsole();
  if (args.size() < 4) return 2;
  float strength = 100.f;
  bool regions = true;
  for (size_t i = 4; i < args.size(); i++) {
    if (args[i] == "--strength" && i + 1 < args.size()) strength = (float)atof(args[++i].c_str());
    if (args[i] == "--no-regions") regions = false;
  }
  std::string err;
  ai::SegResult seg;
  std::vector<float> guide;
  int gw = 0, gh = 0;
  auto t0 = Clock::now();
  if (regions && !ai::segmentFile(args[2], seg, guide, gw, gh, err)) fprintf(stderr, "regions skipped: %s\n", err.c_str());
  double segMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  ai::Recovery rec;
  size_t peak = 0;
  std::atomic<bool> running{true};
  std::thread mon([&] {
    while (running) {
      peak = std::max(peak, processPrivateWorkingSet());
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  bool ok = ai::recoverFile(args[2], seg.valid() ? &seg : nullptr, false, rec, err);
  running = false;
  mon.join();
  if (!ok) {
    fprintf(stderr, "%s\n", err.c_str());
    return 5;
  }
  printf("regions %.0f ms, recovery %.0f ms: %d areas (%d with AI texture), %.1f%% of the image, peak RAM %.0f MB, after %.0f MB\n",
         segMs, rec.ms, rec.areas, rec.textured, rec.blown * 100.f, peak / 1048576.0, processPrivateWorkingSet() / 1048576.0);
  if (!glfwInit()) return 3;
  GLFWwindow* win = createWindow(64, 64, false, err);
  Renderer full;
  if (!win || !full.init(err)) return 3;
  ImageData img;
  if (!loadImage(args[2], full.maxTextureSize(), img, err) || !full.upload(img, err)) return 5;
  if (rec.valid()) full.setRecover(rec.rgba.data(), rec.w, rec.h);
  EditParams p;
  p.recover = strength;
  Geometry g = computeGeometry(p, full.srcW(), full.srcH(), full.orientation(), false);
  std::vector<uint8_t> rgb((size_t)g.outW * g.outH * 3);
  full.exportBegin(p, g.outW, g.outH);
  while (!full.exportStep(rgb.data())) {}
  if (!writeImage(args[3], rgb.data(), g.outW, g.outH, endsWithI(args[3], ".png"), 92, err)) return 7;
  full.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}

// CircleRoom.exe --ai-auto <in> <out.jpg> [--no-layers]
static int runAiAuto(const std::vector<std::string>& args) {
  attachParentConsole();
  if (args.size() < 4) return 2;
  bool layers = true;
  for (size_t i = 4; i < args.size(); i++)
    if (args[i] == "--no-layers") layers = false;
  if (!glfwInit()) return 3;
  std::string err;
  GLFWwindow* win = createWindow(64, 64, false, err);
  Renderer small, full;
  if (!win || !small.init(err) || !full.init(err)) return 3;
  auto t0 = Clock::now();
  ai::SegResult seg;
  std::vector<float> guide;
  int gw = 0, gh = 0;
  if (layers && !ai::segmentFile(args[2], seg, guide, gw, gh, err)) fprintf(stderr, "segmentation skipped: %s\n", err.c_str());
  double segMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  AutoModels m;
  if (!m.load(err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 5;
  }
  EditParams base;
  ai::AutoResult res;
  t0 = Clock::now();
  if (!aiAutoEditFile(small, m, args[2], nullptr, seg.valid() ? &seg : nullptr, seg.valid() ? &guide : nullptr, gw, gh,
                      layers, base, res, err, true)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 5;
  }
  double autoMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  size_t ram = processPrivateWorkingSet();
  m.release();
  ai::ortUnload();
  setLang(Lang::EN);
  printf("scene %s (%.0f%%), candidate %d, regions %.0f ms, auto edit %.0f ms, RAM with models %.0f MB\n",
         sceneName(res.scene), res.sceneConf * 100, res.candidate, segMs, autoMs, ram / 1048576.0);
  std::string ser = serializeParams(res.params, true);
  for (size_t pos = 0; pos < ser.size();) {  // print only the non-default lines
    size_t e = ser.find('\n', pos);
    std::string line = ser.substr(pos, e - pos);
    pos = e + 1;
    EditParams d, one;
    deserializeParams(line, one, true);
    if (!sameParams(d, one)) printf("  %s\n", line.c_str());
  }
  ImageData img;
  if (!loadImage(args[2], full.maxTextureSize(), img, err) || !full.upload(img, err)) return 5;
  if (res.params.recover > 0) {
    ai::Recovery rec;
    if (ai::loadRecoverCache(args[2], rec)) full.setRecover(rec.rgba.data(), rec.w, rec.h);
  }
  if (anyLayer(res.params) && seg.valid()) {
    std::vector<uint8_t> mask;
    ai::buildMasks(seg, guide, gw, gh, res.params.layers, mask);
    full.setMask(mask.data(), gw, gh);
  }
  Geometry g = computeGeometry(res.params, full.srcW(), full.srcH(), full.orientation(), false);
  std::vector<uint8_t> rgb((size_t)g.outW * g.outH * 3);
  full.exportBegin(res.params, g.outW, g.outH);
  while (!full.exportStep(rgb.data())) {}
  if (!writeImage(args[3], rgb.data(), g.outW, g.outH, endsWithI(args[3], ".png"), 92, err)) return 7;
  small.shutdown();
  full.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}

// CircleRoom.exe --ai-mask <in> <region index> <out.png> [--invert] [--feather F]
static int runAiMask(const std::vector<std::string>& args) {
  attachParentConsole();
  if (args.size() < 5) return 2;
  LocalLayer l[kMaxLayers] = {};
  l[0].enabled = 1;
  l[0].region = atoi(args[3].c_str());
  for (size_t i = 5; i < args.size(); i++) {
    if (args[i] == "--invert") l[0].invert = 1;
    if (args[i] == "--feather" && i + 1 < args.size()) l[0].feather = (float)atof(args[++i].c_str());
  }
  ai::SegResult seg;
  std::vector<float> guide;
  std::vector<uint8_t> mask;
  int W = 0, H = 0;
  std::string err;
  auto t0 = Clock::now();
  if (!ai::segmentFile(args[2], seg, guide, W, H, err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 5;
  }
  double segMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  t0 = Clock::now();
  ai::buildMasks(seg, guide, W, H, l, mask);
  double maskMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  std::vector<uint8_t> rgb((size_t)W * H * 3);
  for (size_t i = 0; i < (size_t)W * H; i++) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = mask[i * 4];
  if (!writeImage(args[4], rgb.data(), W, H, true, 90, err)) return 7;
  printf("segment %.0f ms, mask %.0f ms, %dx%d; areas:", segMs, maskMs, W, H);
  for (int r = 0; r < ai::REG_COUNT; r++)
    if (seg.area[r] > 0.01f) printf(" %d=%.0f%%", r, seg.area[r] * 100);
  printf("\n");
  return 0;
}

// CircleRoom.exe --make-style <name> <photosDir>: saves a reference set to styles\<name>.lrs
static int runMakeStyle(const std::vector<std::string>& args) {
  attachParentConsole();
  if (args.size() < 4) return 2;
  ai::Analyzer an;
  an.startFiles(listImages(args[3]), false);
  while (an.running()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  an.poll();
  std::vector<ai::PhotoInfo> refs = an.takeResults();
  if (refs.size() < 5) {
    fprintf(stderr, "need at least 5 photos, got %zu\n", refs.size());
    return 5;
  }
  StyleLibrary lib;
  std::string err;
  if (!lib.save(ai::StyleSet::fromPhotos(args[2], refs), err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 7;
  }
  printf("saved style '%s' with %zu references\n", args[2].c_str(), refs.size());
  return 0;
}

// CircleRoom.exe --ai-style <refsDir> <in> <out.jpg> [--strength 0..100]
static int runAiStyle(const std::vector<std::string>& args) {
  attachParentConsole();
  if (args.size() < 5) {
    fprintf(stderr, "usage: --ai-style refsDir in out.jpg [--strength S]\n");
    return 2;
  }
  float strength = 100.f;
  for (size_t i = 5; i + 1 < args.size(); i++)
    if (args[i] == "--strength") strength = (float)atof(args[i + 1].c_str());
  auto analyse = [](const std::vector<std::string>& files) {
    ai::Analyzer an;
    an.startFiles(files, false);
    while (an.running()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    an.poll();
    return an.takeResults();
  };
  std::vector<ai::PhotoInfo> refs = analyse(listImages(args[2]));
  std::vector<ai::PhotoInfo> target = analyse({args[3]});
  if (refs.empty() || target.empty()) {
    fprintf(stderr, "could not analyse references or target\n");
    return 5;
  }
  ai::StyleSet set = ai::StyleSet::fromPhotos("cli", refs);
  if (!glfwInit()) return 3;
  std::string err;
  GLFWwindow* win = createWindow(64, 64, false, err);
  Renderer small, full;
  if (!win || !small.init(err) || !full.init(err)) {
    fprintf(stderr, "GL init failed: %s\n", err.c_str());
    return 3;
  }
  ai::SmallImage si;
  if (!ai::decodeScaled(args[3], 512, si, nullptr, err)) {
    fprintf(stderr, "decode failed: %s\n", err.c_str());
    return 5;
  }
  ImageData sd;
  sd.w = si.w;
  sd.h = si.h;
  sd.px8 = si.rgb;
  small.upload(sd, err);
  EditParams base;
  ai::StyleResult r = ai::solveStyle(small, set, target[0].hasAi ? target[0].emb : nullptr, base, ai::SG_ALL);
  EditParams p = ai::blendStyle(base, r.params, strength / 100.f);
  printf("refs %zu, tone distance %.4f -> %.4f (%.0f%% closer), %d renders, %.0f ms\n", refs.size(), r.lossBefore,
         r.lossAfter, 100.0 * (1.0 - r.lossAfter / std::max(1e-6f, r.lossBefore)), r.evals, r.ms);
  for (size_t i = 0; i < refs.size(); i++) printf("  weight %.3f  %s\n", r.weights[i], fileName(refs[i].path).c_str());
  printf("%s", serializeParams(p, false).c_str());
  ImageData img;
  if (!loadImage(args[3], full.maxTextureSize(), img, err) || !full.upload(img, err)) return 5;
  Geometry g = computeGeometry(p, full.srcW(), full.srcH(), full.orientation(), false);
  std::vector<uint8_t> rgb((size_t)g.outW * g.outH * 3);
  full.exportBegin(p, g.outW, g.outH);
  while (!full.exportStep(rgb.data())) {}
  if (!writeImage(args[4], rgb.data(), g.outW, g.outH, endsWithI(args[4], ".png"), 92, err)) return 7;
  small.shutdown();
  full.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}

// CircleRoom.exe --ai-rank <folder> [--top N] [--refs <dir>] [--cpu]
static int runAiRank(const std::vector<std::string>& args) {
  attachParentConsole();
  if (args.size() < 3) {
    fprintf(stderr, "usage: --ai-rank folder [--top N] [--refs dir] [--cpu]\n");
    return 2;
  }
  int top = 10;
  std::string refsDir;
  bool gpu = false, dedupe = true;
  for (size_t i = 3; i < args.size(); i++) {
    if (args[i] == "--top" && i + 1 < args.size()) top = atoi(args[++i].c_str());
    else if (args[i] == "--refs" && i + 1 < args.size()) refsDir = args[++i];
    else if (args[i] == "--gpu") gpu = true;
    else if (args[i] == "--all") dedupe = false;
  }
  auto ms = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
  size_t peak = 0;
  auto runJob = [&](ai::Analyzer& an) {
    while (an.running()) {
      peak = std::max(peak, processPrivateWorkingSet());
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    an.poll();
  };
  auto t0 = Clock::now();
  ai::Analyzer an;
  an.startFolder(args[2], gpu);
  runJob(an);
  std::vector<ai::PhotoInfo> photos = an.takeResults();
  printf("analysed %d new / %zu photos in %.0f ms on %s, peak RAM %.0f MB, RAM after %.0f MB\n", an.total(),
         photos.size(), ms(t0), an.message().c_str(), peak / 1048576.0, processPrivateWorkingSet() / 1048576.0);
  std::vector<ai::PhotoInfo> refs;
  if (!refsDir.empty()) {
    ai::Analyzer ar;
    ar.startFiles(listImages(refsDir), gpu);
    runJob(ar);
    refs = ar.takeResults();
    printf("references: %zu\n", refs.size());
  }
  std::vector<int> groups = ai::groupDuplicates(photos);
  ai::RankSettings rs;
  rs.refMode = !refs.empty();
  rs.dedupe = dedupe;
  std::vector<ai::RankEntry> ranked = ai::rankPhotos(photos, groups, refs, rs);
  printf("%-4s %-34s %6s %5s %5s %5s %5s %4s  %s\n", "#", "file", "score", "tech", "aes", "comp", "sim", "grp", "details");
  for (int i = 0; i < (int)ranked.size() && i < top; i++) {
    const ai::RankEntry& e = ranked[i];
    const ai::PhotoInfo& p = photos[e.idx];
    printf("%-4d %-34.34s %6.1f %5.2f %5.2f %5.2f %5.2f %4d  g=%d clipHi=%.3f sharp=%.2f aes=%.2f comp=%.2f thirds=%.2f tilt=%.1f(%.2f) "
           "faces=%d eyes=%.2f reasons=%x\n",
           i + 1, fileName(p.path).c_str(), e.score, e.tech, e.aes, e.comp, e.sim, e.groupSize, e.group, p.feat.clipHi, p.feat.sharp, p.aesthetic,
           p.prompts[ai::P_COMPOSITION], p.feat.thirds, p.feat.tilt, p.feat.tiltConf, p.faces, p.eyesOpen, e.reasons);
  }
  return 0;
}

int main(int, char**) {
  hardenDllSearch();
  std::vector<std::string> args = commandLineArgs();
  if (args.size() >= 2 && args[1] == "--ai-rank") return runAiRank(args);
  if (args.size() >= 2 && args[1] == "--ai-style") return runAiStyle(args);
  if (args.size() >= 2 && args[1] == "--make-style") return runMakeStyle(args);
  if (args.size() >= 2 && args[1] == "--ai-search") return runAiSearch(args);
  if (args.size() >= 2 && args[1] == "--ai-mask") return runAiMask(args);
  if (args.size() >= 2 && args[1] == "--ai-auto") return runAiAuto(args);
  if (args.size() >= 2 && args[1] == "--ai-recover") return runAiRecover(args);
  if (args.size() >= 2 && args[1] == "--ai-tokcheck") return runTokCheck(args);
  if (args.size() >= 2 && args[1] == "--selftest") return runSelfTest(args);
  if (args.size() >= 2 && (args[1] == "--export" || args[1] == "--bench")) return runHeadless(args);

  App a;
  g_app = &a;
  loadSettings(a.S);
  setLang(a.S.lang == 1 ? Lang::TH : Lang::EN);
  if (!glfwInit()) return 1;
  std::string err;
  a.win = createWindow(a.S.winW, a.S.winH, false, err);
  if (!a.win) {
    attachParentConsole();
    fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  {  // keep the window inside the monitor's work area (scale-to-monitor can make it too big)
    int mx, my, mw, mh, ww, wh;
    glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &mx, &my, &mw, &mh);
    glfwGetWindowSize(a.win, &ww, &wh);
    if (mw > 0 && mh > 0 && (ww > mw * 0.95 || wh > mh * 0.92)) {
      ww = std::min(ww, (int)(mw * 0.9));
      wh = std::min(wh, (int)(mh * 0.86));
      glfwSetWindowSize(a.win, ww, wh);
    }
    if (mw > 0) glfwSetWindowPos(a.win, mx + (mw - ww) / 2, my + std::max(30, (mh - wh) / 2));
  }
  if (a.S.maximized) glfwMaximizeWindow(a.win);
  glfwShowWindow(a.win);
  glfwSwapInterval(1);
  g_layerWake = []() { glfwPostEmptyEvent(); };
  glfwSetDropCallback(a.win, [](GLFWwindow*, int n, const char** paths) {
    if (g_app)
      for (int i = 0; i < n; i++) g_app->drops.push_back(paths[i]);
  });
  float sx = 1.f, sy = 1.f;
  glfwGetWindowContentScale(a.win, &sx, &sy);
  a.dpiScale = std::max(1.f, sx);
  a.S.textPct = clampTextPct(a.S.textPct);
  a.scale = a.dpiScale * a.S.textPct / 100.f;

  if (!a.R.init(err)) {
    attachParentConsole();
    fprintf(stderr, "shader error: %s\n", err.c_str());
    return 1;
  }
  a.gpuName = (const char*)glGetString(GL_RENDERER);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  setupFonts(a.scale);
  applyTheme(a.scale);
  ImGui_ImplGlfw_InitForOpenGL(a.win, true);
  ImGui_ImplOpenGL3_Init("#version 330");
  loadPresets(a);
  a.styles.reload();

  for (size_t i = 1; i < args.size(); i++)
    if (args[i].rfind("--", 0) != 0) {
      if (isDirectory(args[i])) {  // a folder opens straight into AI cull
        a.mode = 1;
        a.cull.onDrop({args[i]});
      } else {
        openImage(a, args[i]);
      }
      break;
    }

  int framesLeft = 3;
  while (!glfwWindowShouldClose(a.win)) {
    bool busy = a.load.active || a.exp.stage != 0 || (!a.toast.empty() && Clock::now() < a.toastUntil) ||
                a.cull.busy() || a.styles.creating() || a.segJob.active || a.recJob.active;
    if (framesLeft > 0) glfwPollEvents();
    else if (busy) glfwWaitEventsTimeout(0.05);
    else glfwWaitEvents();  // idle: no CPU/GPU use until something happens
    if (framesLeft <= 0) framesLeft = 3;
    framesLeft--;
    if (glfwGetWindowAttrib(a.win, GLFW_ICONIFIED)) {
      framesLeft = 0;
      continue;
    }

    if (a.load.active && a.load.done) finishLoad(a);
    if (a.segJob.active && a.segJob.done) finishSegmentation(a);
    if (a.recJob.active && a.recJob.done) finishRecover(a);
    updateMasks(a);
    if (a.hasImage && a.layers.sync(a.params, a.R)) a.maskVer++;
    if (a.hasImage && a.heal.sync(a.params, a.R, a.path)) a.maskVer++;
    {
      std::string he = a.heal.takeError();
      if (!he.empty()) showToast(a, std::string(tr(T_MagicFailed)) + ": " + he);
    }
    if (!a.drops.empty()) {
      if (a.mode == 1) a.cull.onDrop(a.drops);
      else if (a.page == PG_LAYERS && a.hasImage)
        for (const std::string& d : a.drops) addImageLayer(a, d);
      else openImage(a, a.drops.front());
      a.drops.clear();
    }
    {
      CullHost host = cullHost(a);
      a.cull.pump(host, a.styles);
      std::string serr;
      if (a.styles.pump(serr)) showToast(a, std::string(tr(T_Saved)) + ": " + a.styles.sets()[a.styles.selected].name);
      else if (!serr.empty()) showToast(a, serr);
    }
    pumpExport(a);
    if (a.exp.stage == 1) framesLeft = std::max(framesLeft, 1);

    if (a.pendingTextPct) {
      applyTextSize(a);
      framesLeft = std::max(framesLeft, 2);
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    buildUI(a);
    ImGui::Render();
    int fbw, fbh;
    glfwGetFramebufferSize(a.win, &fbw, &fbh);
    glViewport(0, 0, fbw, fbh);
    glClearColor(0.1f, 0.1f, 0.1f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(a.win);
  }

  if (a.load.active) a.load.th.join();
  if (a.segJob.th.joinable()) a.segJob.th.join();
  if (a.recJob.th.joinable()) a.recJob.th.join();
  a.cull.shutdown();
  if (a.aiR) {
    a.aiR->shutdown();
    delete a.aiR;
  }
  if (a.exp.th.joinable()) a.exp.th.join();
  saveSidecar(a);
  a.S.maximized = glfwGetWindowAttrib(a.win, GLFW_MAXIMIZED);
  if (!a.S.maximized) {
    int w, h;
    glfwGetWindowSize(a.win, &w, &h);
    float cx = 1.f, cy = 1.f;
    glfwGetWindowContentScale(a.win, &cx, &cy);
    a.S.winW = (int)(w / std::max(1.f, cx));
    a.S.winH = (int)(h / std::max(1.f, cy));
  }
  saveSettings(a.S);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  a.R.shutdown();
  glfwDestroyWindow(a.win);
  glfwTerminate();
  return 0;
}
