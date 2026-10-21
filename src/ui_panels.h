#pragma once
#include <string>

#include "imgui.h"
#include "params.h"

// Palette: warm graphite like a camera body, paper-white type, and one darkroom
// "safelight" red used sparingly for state.
namespace pal {
constexpr ImU32 kCanvas = IM_COL32(18, 17, 16, 255);
constexpr ImU32 kPanel = IM_COL32(28, 27, 25, 255);
constexpr ImU32 kRail = IM_COL32(23, 22, 20, 255);
constexpr ImU32 kCard = IM_COL32(37, 35, 32, 255);
constexpr ImU32 kFrame = IM_COL32(52, 49, 45, 255);
constexpr ImU32 kHover = IM_COL32(45, 43, 39, 255);
constexpr ImU32 kTrack = IM_COL32(78, 74, 67, 255);
constexpr ImU32 kText = IM_COL32(234, 229, 219, 255);
constexpr ImU32 kMuted = IM_COL32(152, 145, 133, 255);
constexpr ImU32 kFaint = IM_COL32(98, 93, 86, 255);
constexpr ImU32 kAccent = IM_COL32(226, 82, 54, 255);
constexpr ImU32 kAccentSoft = IM_COL32(226, 82, 54, 34);
constexpr ImU32 kOnAccent = IM_COL32(255, 246, 238, 255);
constexpr ImU32 kInk = IM_COL32(26, 24, 22, 255);  // text on paper-coloured buttons
}  // namespace pal

extern ImFont* g_fontBold;   // headings (Bahnschrift + Thai bold)
extern ImFont* g_fontSmall;  // captions, hints
extern ImFont* g_fontMono;   // numbers: slider values, section indices
extern ImFont* g_fontTitle;  // page titles, wordmark

// What the panels did this frame.
struct EditResult {
  bool changed = false;  // params changed (re-render)
  int commit = -1;       // i18n key of a finished edit -> history entry
  std::string extra;
};

struct PanelState {
  int curveChannel = 0;
  int curveDrag = -1;
  int gradeSel = 0;
  int hslMode = 0;
};

// Rectangles of UI elements, registered while drawing, that the tutorial can spotlight.
enum TourTarget { TR_OPEN, TR_RAIL, TR_TOOLS, TR_VIEWBAR, TR_EXPORT, TR_CULLTAB, TR_HELP, TR_CFOLDER, TR_CLEFT, TR_CGRID, TR_COUNT };
void tourClear();
void tourSet(int id, ImVec2 a, ImVec2 b);
void tourSetItem(int id);     // rect of the last submitted item
void tourSetWindow(int id);   // rect of the current window / child
bool tourGet(int id, ImVec2& a, ImVec2& b);

void applyTheme(float scale);
// Label + value on one line, a full-width track below. Double-click resets, Ctrl+click types.
bool uiSlider(EditResult& r, int key, const char* id, float* v, float vmin, float vmax, float def, const char* fmt,
              ImU32 gradL = 0, ImU32 gradR = 0, bool hueTrack = false);
// A numbered section ("01  Tone") under a hairline; returns true when its reset link was clicked.
// Always pair with uiCardEnd. uiSectionReset() restarts the numbering (once per page).
bool uiCardBegin(const char* id, const char* title, bool resettable = true);
void uiCardEnd();
void uiSectionReset();
// Text toggle with an underline when on (toolbars).
bool uiTextToggle(const char* label, bool on, const char* tip = nullptr);
bool uiPrimaryButton(const char* label, ImVec2 size = ImVec2(-FLT_MIN, 0));
// Segmented control; returns the clicked index or -1.
int uiSegmented(const char* id, const char* const* labels, int n, int sel, bool fill = true);
void uiHint(const char* text);
void uiHistogram(const unsigned hist[4][256], float scale, float w, float h, bool overlay);
bool uiPill(const char* label, bool selected, ImU32 textCol = 0);

void uiLight(EditParams& p, EditResult& r, bool reset);
void uiWhiteBalance(EditParams& p, EditResult& r, bool reset);
void uiVividness(EditParams& p, EditResult& r, bool reset);
void uiDefinition(EditParams& p, EditResult& r, bool reset);
void uiSharpen(EditParams& p, EditResult& r, bool reset);
void uiNoise(EditParams& p, EditResult& r, bool reset);
void uiVignette(EditParams& p, EditResult& r, bool reset);
void uiGrain(EditParams& p, EditResult& r, bool reset);
void uiToneCurve(EditParams& p, EditResult& r, PanelState& s, const unsigned hist[4][256], float scale, bool reset);
void uiColorMixer(EditParams& p, EditResult& r, PanelState& s, bool reset);
void uiColorGrading(EditParams& p, EditResult& r, PanelState& s, float scale, bool reset);
