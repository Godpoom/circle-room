#include "ai/features.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ai {

static float smooth01(float a, float b, float x) {
  float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3 - 2 * t);
}

static const float* srgbLut() {
  static float lut[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; i++) {
      float c = i / 255.f;
      lut[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    init = true;
  }
  return lut;
}

static void toLab(const uint8_t* p, float& L, float& a, float& b) {
  const float* lut = srgbLut();
  float r = lut[p[0]], g = lut[p[1]], bl = lut[p[2]];
  float X = (0.4124f * r + 0.3576f * g + 0.1805f * bl) / 0.95047f;
  float Y = 0.2126f * r + 0.7152f * g + 0.0722f * bl;
  float Z = (0.0193f * r + 0.1192f * g + 0.9505f * bl) / 1.08883f;
  auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
  float fx = f(X), fy = f(Y), fz = f(Z);
  L = 116.f * fy - 16.f;
  a = 500.f * (fx - fy);
  b = 200.f * (fy - fz);
}

static float hueDeg(const uint8_t* p, float& sat) {
  float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
  float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
  sat = mx > 0 ? d / mx : 0;
  if (d <= 1e-6f) return 0;
  float h = mx == r ? std::fmod((g - b) / d + 6.f, 6.f) : (mx == g ? (b - r) / d + 2.f : (r - g) / d + 4.f);
  return h * 60.f;
}

static const float kBand[9] = {0, 30, 60, 120, 180, 240, 270, 300, 360};

void toneSignature(const uint8_t* px, int w, int h, int stride, ToneSig& out) {
  std::memset(&out, 0, sizeof(out));
  size_t n = (size_t)w * h;
  if (!n) return;
  std::vector<float> Lv(n), Av(n), Bv(n), Cv(n);
  std::vector<int> hist(1001, 0), chist(1001, 0);
  double sumL = 0, sumL2 = 0, sa = 0, sb = 0, zoneA[3] = {0, 0, 0}, zoneB[3] = {0, 0, 0}, zoneW[3] = {0, 0, 0};
  double band[8][4] = {}, bandW[8] = {};
  for (size_t i = 0; i < n; i++) {
    const uint8_t* p = px + i * stride;
    float L, a, b;
    toLab(p, L, a, b);
    Lv[i] = L;
    hist[std::clamp((int)(L * 10.f), 0, 1000)]++;
    float C = std::sqrt(a * a + b * b);
    chist[std::clamp((int)(C * 10.f), 0, 1000)]++;
    sumL += L;
    sumL2 += (double)L * L;
    sa += a;
    sb += b;
    Av[i] = a;
    Bv[i] = b;
    Cv[i] = C;
    float sat;
    float hd = hueDeg(p, sat);
    float cw = smooth01(0.06f, 0.25f, sat);
    if (cw <= 0) continue;
    int k = 0;
    for (int j = 0; j < 8; j++)
      if (hd >= kBand[j]) k = j;
    float t = (hd - kBand[k]) / (kBand[k + 1] - kBand[k]);
    int k2 = (k + 1) % 8;
    float wk[2] = {(1 - t) * cw, t * cw};
    int bk[2] = {k, k2};
    for (int q = 0; q < 2; q++) {
      int B = bk[q];
      float center = kBand[B];
      float off = hd - center;
      if (off > 180) off -= 360;
      if (off < -180) off += 360;
      band[B][1] += C * wk[q];
      band[B][2] += L * wk[q];
      band[B][3] += off / 30.f * wk[q];
      bandW[B] += wk[q];
    }
  }
  auto pct = [&](const std::vector<int>& hh, double q) {
    size_t target = (size_t)(q * (n - 1)), acc = 0;
    for (int i = 0; i <= 1000; i++) {
      acc += hh[i];
      if (acc > target) return i / 10.f;
    }
    return 100.f;
  };
  // Zone colour cast is measured on the least saturated pixels: that is the grading / white
  // balance, while saturated pixels mostly tell what the scene contains (blue sky, grass).
  // The threshold adapts so a strong cast (every pixel tinted) is still measured.
  {
    float thr = std::max(10.f, pct(chist, 0.3));
    for (size_t i = 0; i < n; i++) {
      float L = Lv[i];
      float ws = 1.f - smooth01(0.f, 50.f, L), wh = smooth01(50.f, 100.f, L), wm = std::max(0.f, 1.f - ws - wh);
      float neutral = 1.f - smooth01(thr * 0.5f, thr * 1.5f, Cv[i]);
      float zw[3] = {ws * neutral, wm * neutral, wh * neutral};
      for (int z = 0; z < 3; z++) {
        zoneA[z] += Av[i] * zw[z];
        zoneB[z] += Bv[i] * zw[z];
        zoneW[z] += zw[z];
      }
    }
  }
  const double qs[7] = {0.01, 0.05, 0.25, 0.5, 0.75, 0.95, 0.99};
  for (int i = 0; i < 7; i++) out.v[i] = pct(hist, qs[i]) / 100.f;
  double mean = sumL / n;
  out.v[7] = (float)(std::sqrt(std::max(0.0, sumL2 / n - mean * mean)) / 100.0);
  // Local contrast: mean |L - box blur(L)| via an integral image.
  {
    int r = std::max(2, std::max(w, h) / 40);
    std::vector<double> I((size_t)(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; y++) {
      double row = 0;
      for (int x = 0; x < w; x++) {
        row += Lv[(size_t)y * w + x];
        I[(size_t)(y + 1) * (w + 1) + x + 1] = I[(size_t)y * (w + 1) + x + 1] + row;
      }
    }
    double acc = 0;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int x0 = std::max(0, x - r), x1 = std::min(w, x + r + 1), y0 = std::max(0, y - r), y1 = std::min(h, y + r + 1);
        double s = I[(size_t)y1 * (w + 1) + x1] - I[(size_t)y0 * (w + 1) + x1] - I[(size_t)y1 * (w + 1) + x0] +
                   I[(size_t)y0 * (w + 1) + x0];
        acc += std::fabs(Lv[(size_t)y * w + x] - s / ((x1 - x0) * (y1 - y0)));
      }
    out.v[8] = (float)(acc / n / 100.0);
  }
  out.v[9] = (float)(sa / n / 100.0);
  out.v[10] = (float)(sb / n / 100.0);
  for (int z = 0; z < 3; z++) {
    out.v[11 + z * 2] = zoneW[z] > 1 ? (float)(zoneA[z] / zoneW[z] / 100.0) : 0.f;
    out.v[12 + z * 2] = zoneW[z] > 1 ? (float)(zoneB[z] / zoneW[z] / 100.0) : 0.f;
  }
  out.v[17] = pct(chist, 0.25) / 100.f;
  out.v[18] = pct(chist, 0.5) / 100.f;
  out.v[19] = pct(chist, 0.9) / 100.f;
  for (int B = 0; B < 8; B++) {
    float* o = &out.v[20 + B * 4];
    o[0] = (float)(bandW[B] / n);
    if (bandW[B] > 1e-3) {
      o[1] = (float)(band[B][1] / bandW[B] / 100.0);
      o[2] = (float)(band[B][2] / bandW[B] / 100.0);
      o[3] = (float)(band[B][3] / bandW[B]);
    }
  }
}

float toneDistance(const ToneSig& a, const ToneSig& b) {
  double s = 0;
  auto add = [&](int i, float w) {
    double d = a.v[i] - b.v[i];
    s += w * d * d;
  };
  for (int i = 0; i <= 6; i++) add(i, 3.f);
  add(7, 2.f);
  add(8, 6.f);
  add(9, 6.f);
  add(10, 6.f);
  for (int i = 11; i <= 16; i++) add(i, 3.f);
  for (int i = 17; i <= 19; i++) add(i, 3.f);
  for (int B = 0; B < 8; B++) {
    int o = 20 + B * 4;
    float both = std::min(1.f, 8.f * std::min(a.v[o], b.v[o]));
    add(o, 0.5f);
    add(o + 1, 3.f * both);
    add(o + 2, 1.f * both);
    add(o + 3, 0.5f * both);
  }
  return (float)std::sqrt(s);
}

float styleDistance(const ToneSig& a, const ToneSig& b) {
  // Style, not content: tonal shape, contrast and colour cast per tonal zone dominate;
  // per-hue terms are weak because they depend on what is in the picture.
  double s = 0;
  auto add = [&](int i, float w) {
    double d = a.v[i] - b.v[i];
    s += w * d * d;
  };
  for (int i = 0; i <= 6; i++) add(i, 4.f);
  add(7, 3.f);
  add(8, 6.f);
  add(9, 0.7f);  // whole-image average colour is mostly content
  add(10, 0.7f);
  for (int i = 11; i <= 16; i++) add(i, 6.f);
  for (int i = 17; i <= 19; i++) add(i, 2.f);
  for (int B = 0; B < 8; B++) {
    int o = 20 + B * 4;
    float both = std::min(1.f, 8.f * std::min(a.v[o], b.v[o]));
    add(o + 1, 1.f * both);
    add(o + 2, 0.4f * both);
    add(o + 3, 0.2f * both);
  }
  return (float)std::sqrt(s);
}

// ---------------------------------------------------------------- sharpness / noise
static void grayOf(const SmallImage& im, std::vector<float>& g) {
  g.resize((size_t)im.w * im.h);
  for (size_t i = 0; i < g.size(); i++) {
    const uint8_t* p = &im.rgb[i * 3];
    g[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.f;
  }
}

static void laplacian(const std::vector<float>& g, int w, int h, std::vector<float>& lap) {
  lap.assign(g.size(), 0.f);
  for (int y = 1; y < h - 1; y++)
    for (int x = 1; x < w - 1; x++) {
      size_t i = (size_t)y * w + x;
      lap[i] = 4 * g[i] - g[i - 1] - g[i + 1] - g[i - w] - g[i + w];
    }
}

static float tileEnergy(const std::vector<float>& lap, int w, int x0, int y0, int x1, int y1) {
  double s = 0, s2 = 0;
  int n = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      float v = lap[(size_t)y * w + x];
      s += v;
      s2 += (double)v * v;
      n++;
    }
  if (!n) return 0;
  double m = s / n;
  return (float)(s2 / n - m * m);
}

float regionSharpness(const SmallImage& im, int x0, int y0, int x1, int y1) {
  x0 = std::clamp(x0, 1, im.w - 1);
  x1 = std::clamp(x1, x0 + 1, im.w - 1);
  y0 = std::clamp(y0, 1, im.h - 1);
  y1 = std::clamp(y1, y0 + 1, im.h - 1);
  std::vector<float> g, lap;
  grayOf(im, g);
  laplacian(g, im.w, im.h, lap);
  return std::log10(1e-7f + tileEnergy(lap, im.w, x0, y0, x1, y1));
}

// ---------------------------------------------------------------- composition helpers
static void saliency(const SmallImage& im, std::vector<float>& S, int& sw, int& sh) {
  // Frequency-tuned saliency (Achanta 2009) on a ~96 px copy: distance of the
  // blurred Lab colour from the image mean.
  SmallImage s;
  fitLongEdge(im, 96, s);
  sw = s.w;
  sh = s.h;
  size_t n = (size_t)sw * sh;
  std::vector<float> L(n), A(n), B(n);
  double mL = 0, mA = 0, mB = 0;
  for (size_t i = 0; i < n; i++) {
    toLab(&s.rgb[i * 3], L[i], A[i], B[i]);
    mL += L[i];
    mA += A[i];
    mB += B[i];
  }
  mL /= n;
  mA /= n;
  mB /= n;
  auto blur = [&](std::vector<float>& c) {
    std::vector<float> t(c.size());
    for (int pass = 0; pass < 2; pass++) {
      for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
          float a = c[(size_t)y * sw + std::max(0, x - 1)], b = c[(size_t)y * sw + x], d = c[(size_t)y * sw + std::min(sw - 1, x + 1)];
          t[(size_t)y * sw + x] = (a + 2 * b + d) * 0.25f;
        }
      for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
          float a = t[(size_t)std::max(0, y - 1) * sw + x], b = t[(size_t)y * sw + x], d = t[(size_t)std::min(sh - 1, y + 1) * sw + x];
          c[(size_t)y * sw + x] = (a + 2 * b + d) * 0.25f;
        }
    }
  };
  blur(L);
  blur(A);
  blur(B);
  S.resize(n);
  float mx = 1e-6f;
  for (size_t i = 0; i < n; i++) {
    float dl = L[i] - (float)mL, da = A[i] - (float)mA, db = B[i] - (float)mB;
    S[i] = std::sqrt(dl * dl + da * da + db * db);
    mx = std::max(mx, S[i]);
  }
  for (float& v : S) v /= mx;
}

static void tiltEstimate(const std::vector<float>& g, int w, int h, float& tilt, float& conf) {
  // Histogram of edge (line) directions relative to the nearest axis, within +-12 degrees.
  const int bins = 97;  // 0.25 deg bins over [-12, 12]
  std::vector<double> hist(bins, 0.0);
  double total = 0;
  for (int y = 1; y < h - 1; y++)
    for (int x = 1; x < w - 1; x++) {
      size_t i = (size_t)y * w + x;
      float gx = (g[i + 1 - w] + 2 * g[i + 1] + g[i + 1 + w]) - (g[i - 1 - w] + 2 * g[i - 1] + g[i - 1 + w]);
      float gy = (g[i - 1 + w] + 2 * g[i + w] + g[i + 1 + w]) - (g[i - 1 - w] + 2 * g[i - w] + g[i + 1 - w]);
      float m2 = gx * gx + gy * gy;
      if (m2 < 0.02f) continue;
      float ang = std::atan2(gy, gx) * 57.29578f + 90.f;  // line direction
      ang = std::fmod(ang + 360.f, 90.f);
      if (ang >= 45.f) ang -= 90.f;
      total += m2;
      if (ang < -12.f || ang > 12.f) continue;
      hist[std::clamp((int)std::lround((ang + 12.f) * 4.f), 0, bins - 1)] += m2;
    }
  std::vector<double> sm(bins, 0.0);
  for (int i = 0; i < bins; i++)
    for (int k = -3; k <= 3; k++) {
      int j = i + k;
      if (j >= 0 && j < bins) sm[i] += hist[j] * std::exp(-k * k / 4.5);
    }
  int best = (int)(std::max_element(sm.begin(), sm.end()) - sm.begin());
  double win = 0;
  for (int k = -4; k <= 4; k++)
    if (best + k >= 0 && best + k < bins) win += hist[best + k];
  tilt = best / 4.f - 12.f;
  conf = total > 0 ? (float)std::min(1.0, win / total * 4.0) : 0.f;  // >25% of all edge energy -> 1
}

void computeFeatures(const SmallImage& im, Features& f) {
  f = Features();
  if (im.w < 8 || im.h < 8) return;
  std::vector<float> g, lap;
  grayOf(im, g);
  laplacian(g, im.w, im.h, lap);
  // Tiles: sharpest 10% (bokeh-friendly) and flattest 15% (noise).
  const int T = 8;
  std::vector<float> energy;
  std::vector<std::pair<float, int>> flat;
  for (int ty = 0; ty < T; ty++)
    for (int tx = 0; tx < T; tx++) {
      int x0 = std::max(1, tx * im.w / T), x1 = std::min(im.w - 1, (tx + 1) * im.w / T);
      int y0 = std::max(1, ty * im.h / T), y1 = std::min(im.h - 1, (ty + 1) * im.h / T);
      float e = tileEnergy(lap, im.w, x0, y0, x1, y1);
      energy.push_back(e);
      flat.push_back({e, ty * T + tx});
    }
  std::sort(energy.begin(), energy.end());
  f.sharp = std::log10(1e-7f + energy[(size_t)(energy.size() * 0.9)]);
  std::sort(flat.begin(), flat.end());
  std::vector<float> mad;
  for (size_t k = 0; k < flat.size() * 15 / 100 + 1; k++) {
    int t = flat[k].second, tx = t % T, ty = t / T;
    for (int y = std::max(1, ty * im.h / T); y < std::min(im.h - 1, (ty + 1) * im.h / T); y++)
      for (int x = std::max(1, tx * im.w / T); x < std::min(im.w - 1, (tx + 1) * im.w / T); x++)
        mad.push_back(std::fabs(lap[(size_t)y * im.w + x]));
  }
  if (!mad.empty()) {
    std::nth_element(mad.begin(), mad.begin() + mad.size() / 2, mad.end());
    f.noise = mad[mad.size() / 2];
  }
  // Exposure
  size_t n = g.size(), hi = 0, lo = 0;
  std::vector<int> hist(256, 0);
  double sum = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t* p = &im.rgb[i * 3];
    if (p[0] >= 250 || p[1] >= 250 || p[2] >= 250) hi++;
    int v = (int)(g[i] * 255.f);
    if (v <= 5) lo++;
    hist[std::clamp(v, 0, 255)]++;
    sum += g[i];
  }
  f.clipHi = (float)hi / n;
  f.clipLo = (float)lo / n;
  f.meanL = (float)(sum / n);
  size_t acc = 0;
  int p1 = 0, p99 = 255;
  for (int i = 0; i < 256; i++) {
    acc += hist[i];
    if (acc < n / 100) p1 = i;
    if (acc < n * 99 / 100) p99 = i;
  }
  f.dynRange = (p99 - p1) / 255.f;
  // Composition from saliency
  std::vector<float> S;
  int sw, sh;
  saliency(im, S, sw, sh);
  double mean = 0, var = 0;
  for (float v : S) mean += v;
  mean /= S.size();
  for (float v : S) var += (v - mean) * (v - mean);
  float thr = (float)(mean + std::sqrt(var / S.size()));
  double tot = 0, cx = 0, cy = 0, border = 0, left = 0, maskN = 0;
  int bw = std::max(1, sw * 4 / 100), bh = std::max(1, sh * 4 / 100);
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float v = S[(size_t)y * sw + x];
      double m = v * v;
      tot += m;
      cx += m * (x + 0.5) / sw;
      cy += m * (y + 0.5) / sh;
      if (x < sw / 2) left += m;
      if (v > thr) {
        maskN++;
        if (x < bw || x >= sw - bw || y < bh || y >= sh - bh) border += 1;
      }
    }
  if (tot > 0) {
    cx /= tot;
    cy /= tot;
    f.subjectSize = (float)(maskN / S.size());
    f.edgeCut = maskN > 0 && f.subjectSize < 0.5f ? (float)(border / maskN) : 0.f;
    f.balance = (float)std::fabs(2.0 * left / tot - 1.0);
    double dT = 1e9;
    for (int i = 1; i <= 2; i++)
      for (int j = 1; j <= 2; j++) dT = std::min(dT, std::hypot(cx - i / 3.0, cy - j / 3.0));
    double dC = std::hypot(cx - 0.5, cy - 0.5);
    f.thirds = (float)std::max(std::exp(-dT * dT / (2 * 0.07 * 0.07)), 0.7 * std::exp(-dC * dC / (2 * 0.07 * 0.07)));
  }
  // Horizon / verticals tilt on a ~384 px copy
  SmallImage t;
  fitLongEdge(im, 384, t);
  std::vector<float> tg;
  grayOf(t, tg);
  tiltEstimate(tg, t.w, t.h, f.tilt, f.tiltConf);
  float tiltPen = f.tiltConf * smooth01(0.8f, 3.f, std::fabs(f.tilt));
  f.compHeur = 0.45f * f.thirds + 0.25f * (1.f - smooth01(0.1f, 0.4f, f.edgeCut)) +
               0.15f * (1.f - 0.5f * smooth01(0.3f, 0.8f, f.balance)) + 0.15f * (1.f - tiltPen);
}

}  // namespace ai
