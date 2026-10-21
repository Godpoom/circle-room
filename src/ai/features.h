// Model-free image measurements: sharpness, exposure, noise, composition
// heuristics and a compact "tone signature" used for reference matching.
#pragma once
#include <cstdint>

#include "ai/wic_decode.h"

namespace ai {

constexpr int kToneDim = 52;
// Layout: [0..6] L* percentiles 1,5,25,50,75,95,99  [7] L* std  [8] local contrast
// [9,10] mean a*,b*  [11..16] a*,b* in shadows/midtones/highlights  [17..19] chroma p25,p50,p90
// [20 + band*4 + k] for 8 HSL bands: k=0 pixel share, 1 chroma, 2 L*, 3 hue offset (/30 deg)
struct ToneSig {
  float v[kToneDim];
};

// stride: 3 for RGB, 4 for RGBA buffers.
void toneSignature(const uint8_t* px, int w, int h, int stride, ToneSig& out);
// Weighted distance between signatures (colour bands only count where both images have them).
float toneDistance(const ToneSig& a, const ToneSig& b);
// Distance used when matching an edit to references: ignores what colours the scene contains.
float styleDistance(const ToneSig& a, const ToneSig& b);

struct Features {
  float sharp = 0;       // log10 of Laplacian energy in the sharpest tiles (analysis size 768 px)
  float noise = 0;       // Laplacian MAD in the flattest tiles
  float clipHi = 0, clipLo = 0, meanL = 0, dynRange = 0;
  float thirds = 0;      // 0..1, subject near a rule-of-thirds point (or centred, scored lower)
  float edgeCut = 0;     // share of subject saliency touching the frame border
  float balance = 0;     // 0 = balanced left/right
  float subjectSize = 0; // share of frame covered by the salient subject
  float tilt = 0;        // degrees of the dominant near-horizontal/vertical lines
  float tiltConf = 0;    // 0..1
  float compHeur = 0;    // combined 0..1 composition heuristic
};

void computeFeatures(const SmallImage& im, Features& f);
// Sharpness of a sub-rectangle (e.g. a face), same scale as Features::sharp.
float regionSharpness(const SmallImage& im, int x0, int y0, int x1, int y1);

}  // namespace ai
