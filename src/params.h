// Non-destructive edit settings. Plain-old-data (all 4-byte fields) so it can be
// copied and compared with memcmp; history keeps snapshots of this struct only.
#pragma once
#include <string>
#include <vector>

constexpr int kMaxCurvePts = 16;
constexpr int kHslBands = 8;

struct CurvePt { float x, y; };
struct Curve { int n; CurvePt p[kMaxCurvePts]; };
struct GradeWheel { float hue, sat, lum; };  // hue 0..360, sat 0..100, lum -100..100

constexpr int kMaxLayers = 4;
// Local adjustment restricted to an AI-detected region (ai::Region); values -100..100, exposure in EV.
struct LocalLayer {
  int enabled, region, invert;
  float feather;  // 0..100
  float exposure, contrast, highlights, shadows, temp, tint, saturation, clarity;
};

enum { GRADE_SHADOWS, GRADE_MIDTONES, GRADE_HIGHLIGHTS, GRADE_GLOBAL, GRADE_COUNT };
enum { CURVE_MASTER, CURVE_R, CURVE_G, CURVE_B, CURVE_COUNT };

struct EditParams {
  // Basic (sliders -100..100 unless noted)
  float temp, tint;
  float exposure;  // EV -5..5
  float contrast, highlights, shadows, whites, blacks;
  float texture, clarity, dehaze;
  float vibrance, saturation;
  // Tone curve (perceptual 0..1)
  Curve curve[CURVE_COUNT];
  // Color mixer
  float hslHue[kHslBands], hslSat[kHslBands], hslLum[kHslBands];
  // Color grading
  GradeWheel grade[GRADE_COUNT];
  float gradeBlending, gradeBalance;  // 0..100, -100..100
  // Detail
  float sharpAmount, sharpRadius, sharpMasking;  // 0..150, 0.5..3, 0..100
  float nrLum, nrColor;                          // 0..100
  // Effects
  float vigAmount, vigMidpoint, vigFeather, vigRoundness;
  float grainAmount, grainSize, grainRoughness;
  // Geometry
  int rotate90, flipH, flipV;
  float cropX, cropY, cropW, cropH;  // normalized in the (rotated) frame
  float straighten;                  // degrees -45..45
  // AI local adjustments (image specific: kept like geometry when presets/styles apply)
  LocalLayer layers[kMaxLayers];
  float recover;  // AI highlight recovery 0..100 (image specific)

  EditParams();
};

bool sameParams(const EditParams& a, const EditParams& b);
bool sameCurves(const EditParams& a, const EditParams& b);
bool isDefault(const EditParams& p);
void copyGeometry(EditParams& dst, const EditParams& src);
void resetGeometry(EditParams& p);
void copyLayers(EditParams& dst, const EditParams& src);
bool anyLayer(const EditParams& p);

// Sidecar next to each photo ("photo.jpg.pedit"). Files from older builds (".lre") are still
// read, and replaced by the new name on the next save.
constexpr const char* kSidecarExt = ".pedit";
constexpr const char* kLegacySidecarExt = ".lre";
constexpr const char* kLookExt = ".plook";        // saved looks (user presets)
constexpr const char* kLegacyLookExt = ".lrp";
std::string sidecarPath(const std::string& photo);
bool readSidecar(const std::string& photo, std::string& text);
void writeSidecar(const std::string& photo, const std::string& text);  // empty text removes it

std::string serializeParams(const EditParams& p, bool includeGeometry);
// Applies only the keys present in text on top of p.
void deserializeParams(const std::string& text, EditParams& p, bool includeGeometry);

struct Preset {
  std::string nameEn, nameTh;
  std::string data;
  bool builtin = true;
};
std::vector<Preset> builtinPresets();
// Resets all tone/colour settings, then applies the preset; geometry is kept.
void applyPreset(const Preset& pr, EditParams& p);
