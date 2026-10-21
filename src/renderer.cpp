#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "curves.h"
#include "gl_loader.h"
#include "shaders.h"

static const int kExportTile = 1024;
static const int kExportPad = 8;

// ---------- small affine helpers ----------
static M3 affine(float a, float b, float c, float d, float e, float f) { return M3{{a, b, c, d, e, f, 0, 0, 1}}; }
static M3 identity() { return affine(1, 0, 0, 0, 1, 0); }
static M3 mul(const M3& A, const M3& B) {
  M3 r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      float s = 0;
      for (int k = 0; k < 3; k++) s += A.m[i * 3 + k] * B.m[k * 3 + j];
      r.m[i * 3 + j] = s;
    }
  return r;
}
static void apply(const M3& M, float x, float y, float& ox, float& oy) {
  ox = M.m[0] * x + M.m[1] * y + M.m[2];
  oy = M.m[3] * x + M.m[4] * y + M.m[5];
}

static M3 orientMatrix(int o) {
  switch (o) {
    case 2: return affine(-1, 0, 1, 0, 1, 0);
    case 3: return affine(-1, 0, 1, 0, -1, 1);
    case 4: return affine(1, 0, 0, 0, -1, 1);
    case 5: return affine(0, 1, 0, 1, 0, 0);
    case 6: return affine(0, 1, 0, -1, 0, 1);
    case 7: return affine(0, -1, 1, -1, 0, 1);
    case 8: return affine(0, -1, 1, 1, 0, 0);
    default: return identity();
  }
}

void frameDims(const EditParams& p, int srcW, int srcH, int orient, int& w, int& h) {
  w = srcW;
  h = srcH;
  if (orient >= 5) std::swap(w, h);
  if (p.rotate90 & 1) std::swap(w, h);
}

// Straighten rotation about the frame centre, in pixel space, for a crop rect.
static M3 outToFrameMatrix(float cx, float cy, float cw, float ch, float angleDeg, int W, int H) {
  M3 C = affine(cw, 0, cx, 0, ch, cy);
  M3 S = affine((float)W, 0, 0, 0, (float)H, 0);
  M3 Si = affine(1.f / W, 0, 0, 0, 1.f / H, 0);
  float a = angleDeg * 3.14159265f / 180.f, cs = std::cos(a), sn = std::sin(a);
  float hx = W * 0.5f, hy = H * 0.5f;
  M3 R = affine(cs, -sn, hx - cs * hx + sn * hy, sn, cs, hy - sn * hx - cs * hy);
  return mul(Si, mul(R, mul(S, C)));
}

Geometry computeGeometry(const EditParams& p, int srcW, int srcH, int orient, bool fullFrame) {
  Geometry g;
  frameDims(p, srcW, srcH, orient, g.frameW, g.frameH);
  float cx = fullFrame ? 0.f : p.cropX, cy = fullFrame ? 0.f : p.cropY;
  float cw = fullFrame ? 1.f : p.cropW, ch = fullFrame ? 1.f : p.cropH;
  g.outToFrame = outToFrameMatrix(cx, cy, cw, ch, p.straighten, g.frameW, g.frameH);
  M3 F = identity();
  if (p.flipH) F = mul(F, affine(-1, 0, 1, 0, 1, 0));
  if (p.flipV) F = mul(F, affine(1, 0, 0, 0, -1, 1));
  M3 Rk = identity();
  for (int i = 0; i < (p.rotate90 & 3); i++) Rk = mul(Rk, affine(0, 1, 0, -1, 0, 1));
  g.outToOriented = mul(Rk, mul(F, g.outToFrame));
  g.outToSrc = mul(orientMatrix(orient), g.outToOriented);
  g.outW = std::max(1, (int)std::lround(cw * g.frameW));
  g.outH = std::max(1, (int)std::lround(ch * g.frameH));
  return g;
}

bool cropFits(const EditParams& p, int W, int H) {
  M3 M = outToFrameMatrix(p.cropX, p.cropY, p.cropW, p.cropH, p.straighten, W, H);
  const float eps = 1e-4f;
  for (int i = 0; i < 4; i++) {
    float x, y;
    apply(M, (float)(i & 1), (float)(i >> 1), x, y);
    if (x < -eps || y < -eps || x > 1 + eps || y > 1 + eps) return false;
  }
  return true;
}

void constrainCrop(EditParams& p, int W, int H) {
  p.cropW = std::clamp(p.cropW, 0.01f, 1.f);
  p.cropH = std::clamp(p.cropH, 0.01f, 1.f);
  p.cropX = std::clamp(p.cropX, 0.f, 1.f - p.cropW);
  p.cropY = std::clamp(p.cropY, 0.f, 1.f - p.cropH);
  if (cropFits(p, W, H)) return;
  for (int pass = 0; pass < 2; pass++) {
    float mx = p.cropX + p.cropW * 0.5f, my = p.cropY + p.cropH * 0.5f;
    if (pass == 1) mx = my = 0.5f;  // centre could not hold any crop: recentre
    float w0 = p.cropW, h0 = p.cropH, lo = 0.f, hi = 1.f;
    EditParams t = p;
    for (int it = 0; it < 24; it++) {
      float s = 0.5f * (lo + hi);
      t.cropW = w0 * s;
      t.cropH = h0 * s;
      t.cropX = mx - t.cropW * 0.5f;
      t.cropY = my - t.cropH * 0.5f;
      (cropFits(t, W, H) ? lo : hi) = s;
    }
    if (lo > 0.02f || pass == 1) {
      p.cropW = w0 * lo;
      p.cropH = h0 * lo;
      p.cropX = mx - p.cropW * 0.5f;
      p.cropY = my - p.cropH * 0.5f;
      return;
    }
  }
}

// ---------- white balance (Bradford adaptation in linear sRGB) ----------
static void planckXY(double T, double& x, double& y) {
  T = std::clamp(T, 1667.0, 25000.0);
  double t1 = 1e3 / T, t2 = t1 * t1, t3 = t2 * t1;
  x = T <= 4000 ? -0.2661239 * t3 - 0.2343589 * t2 + 0.8776956 * t1 + 0.179910
                : -3.0258469 * t3 + 2.1070379 * t2 + 0.2226347 * t1 + 0.240390;
  double x2 = x * x, x3 = x2 * x;
  if (T <= 2222) y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
  else if (T <= 4000) y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
  else y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
}

struct D3 { double m[9]; };
static D3 dmul(const D3& A, const D3& B) {
  D3 r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      double s = 0;
      for (int k = 0; k < 3; k++) s += A.m[i * 3 + k] * B.m[k * 3 + j];
      r.m[i * 3 + j] = s;
    }
  return r;
}
static D3 dinv(const D3& a) {
  const double* m = a.m;
  double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
  double id = 1.0 / det;
  return D3{{(m[4] * m[8] - m[5] * m[7]) * id, (m[2] * m[7] - m[1] * m[8]) * id, (m[1] * m[5] - m[2] * m[4]) * id,
             (m[5] * m[6] - m[3] * m[8]) * id, (m[0] * m[8] - m[2] * m[6]) * id, (m[2] * m[3] - m[0] * m[5]) * id,
             (m[3] * m[7] - m[4] * m[6]) * id, (m[1] * m[6] - m[0] * m[7]) * id, (m[0] * m[4] - m[1] * m[3]) * id}};
}

static void whiteBalanceMatrix(float temp, float tint, float out[9]) {
  // +temp: assume a bluer illuminant and adapt it to D65 -> warmer image.
  double ks = 6500.0 * std::pow(2.0, temp / 100.0);
  double xs, ys, xd, yd;
  planckXY(ks, xs, ys);
  planckXY(6500.0, xd, yd);
  ys += tint / 100.0 * 0.025;  // +tint: assume greener light -> magenta shift
  const D3 B = {{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296}};
  const D3 toXYZ = {{0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505}};
  double src[3] = {xs / ys, 1.0, (1 - xs - ys) / ys}, dst[3] = {xd / yd, 1.0, (1 - xd - yd) / yd};
  double ls[3], ld[3];
  for (int i = 0; i < 3; i++) {
    ls[i] = B.m[i * 3] * src[0] + B.m[i * 3 + 1] * src[1] + B.m[i * 3 + 2] * src[2];
    ld[i] = B.m[i * 3] * dst[0] + B.m[i * 3 + 1] * dst[1] + B.m[i * 3 + 2] * dst[2];
  }
  D3 D = {{ld[0] / ls[0], 0, 0, 0, ld[1] / ls[1], 0, 0, 0, ld[2] / ls[2]}};
  D3 M = dmul(dinv(toXYZ), dmul(dinv(B), dmul(D, dmul(B, toXYZ))));
  double y = 0;
  for (int i = 0; i < 3; i++) y += toXYZ.m[3 + i] * (M.m[i * 3] + M.m[i * 3 + 1] + M.m[i * 3 + 2]);
  for (int i = 0; i < 9; i++) out[i] = (float)(M.m[i] / y);
}

static void hsvToRgb(float h, float s, float v, float out[3]) {
  for (int i = 0; i < 3; i++) {
    float k = std::fmod(h * 6.f + (i == 0 ? 5.f : (i == 1 ? 3.f : 1.f)), 6.f);
    float c = std::clamp(std::min(k, 4.f - k), 0.f, 1.f);
    out[i] = v * (1.f - s * c);
  }
}

// ---------- GL plumbing ----------
static unsigned compile(unsigned type, const char* src, std::string& err) {
  unsigned s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    err += log;
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static unsigned link(const char* vs, const char* fs, std::string& err) {
  unsigned v = compile(GL_VERTEX_SHADER, vs, err), f = compile(GL_FRAGMENT_SHADER, fs, err);
  if (!v || !f) return 0;
  unsigned p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    err += log;
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static void setTexParams(bool mip) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool Renderer::init(std::string& err) {
  progA_ = link(kVertexShader, kDevelopShader, err);
  progB_ = link(kVertexShader, kFinishShader, err);
  if (!progA_ || !progB_) return false;
  glGenVertexArrays(1, &vao_);
  glGenTextures(1, &curveTex_);
  glBindTexture(GL_TEXTURE_2D, curveTex_);
  setTexParams(false);
  GLint mt = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);
  if (mt > 0) maxTex_ = mt;
  return true;
}

void Renderer::freeTarget(Target& t) {
  if (t.fboA) glDeleteFramebuffers(1, &t.fboA);
  if (t.fboB) glDeleteFramebuffers(1, &t.fboB);
  if (t.texA) glDeleteTextures(1, &t.texA);
  if (t.texB) glDeleteTextures(1, &t.texB);
  t = Target();
}

void Renderer::shutdown() {
  for (auto& t : view_) freeTarget(t);
  freeTarget(hist_);
  freeTarget(exp_);
  clearMask();
  clearRecover();
  if (srcTex_) glDeleteTextures(1, &srcTex_);
  if (curveTex_) glDeleteTextures(1, &curveTex_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (progA_) glDeleteProgram(progA_);
  if (progB_) glDeleteProgram(progB_);
  srcTex_ = curveTex_ = vao_ = progA_ = progB_ = 0;
}

// Uploads one mip level as RGBA strips so the driver never needs a full-size staging copy.
template <class T>
static void uploadLevel(int level, int w, int h, const T* rgb, bool is16) {
  GLenum type = is16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
  if (!glpTexStorage2D) glTexImage2D(GL_TEXTURE_2D, level, is16 ? GL_RGBA16 : GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, type, nullptr);
  const T opaque = is16 ? (T)65535 : (T)255;
  const int strip = std::max(1, 262144 / std::max(1, w));
  std::vector<T> buf((size_t)w * std::min(strip, h) * 4);
  for (int y0 = 0; y0 < h; y0 += strip) {
    int rows = std::min(strip, h - y0);
    const T* s = rgb + (size_t)y0 * w * 3;
    for (size_t i = 0, n = (size_t)w * rows; i < n; i++) {
      buf[i * 4] = s[i * 3];
      buf[i * 4 + 1] = s[i * 3 + 1];
      buf[i * 4 + 2] = s[i * 3 + 2];
      buf[i * 4 + 3] = opaque;
    }
    glTexSubImage2D(GL_TEXTURE_2D, level, 0, y0, w, rows, GL_RGBA, type, buf.data());
  }
}

// 2x2 box downsample; 8-bit data is averaged in linear light like the GPU would.
static void downsample8(const uint8_t* s, int w, int h, std::vector<uint8_t>& d, int nw, int nh) {
  static uint16_t s2l[256];
  static uint8_t l2s[4096];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; i++) {
      double c = i / 255.0;
      s2l[i] = (uint16_t)std::lround((c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)) * 65535.0);
    }
    for (int i = 0; i < 4096; i++) {
      double l = (i + 0.5) / 4096.0;
      l2s[i] = (uint8_t)std::lround((l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055) * 255.0);
    }
    init = true;
  }
  d.resize((size_t)nw * nh * 3);
  for (int y = 0; y < nh; y++) {
    const uint8_t* r0 = s + (size_t)std::min(2 * y, h - 1) * w * 3;
    const uint8_t* r1 = s + (size_t)std::min(2 * y + 1, h - 1) * w * 3;
    uint8_t* o = &d[(size_t)y * nw * 3];
    for (int x = 0; x < nw; x++) {
      int x0 = std::min(2 * x, w - 1) * 3, x1 = std::min(2 * x + 1, w - 1) * 3;
      for (int c = 0; c < 3; c++) {
        unsigned sum = s2l[r0[x0 + c]] + s2l[r0[x1 + c]] + s2l[r1[x0 + c]] + s2l[r1[x1 + c]];
        o[x * 3 + c] = l2s[sum >> 6];
      }
    }
  }
}

static void downsample16(const uint16_t* s, int w, int h, std::vector<uint16_t>& d, int nw, int nh) {
  d.resize((size_t)nw * nh * 3);
  for (int y = 0; y < nh; y++) {
    const uint16_t* r0 = s + (size_t)std::min(2 * y, h - 1) * w * 3;
    const uint16_t* r1 = s + (size_t)std::min(2 * y + 1, h - 1) * w * 3;
    uint16_t* o = &d[(size_t)y * nw * 3];
    for (int x = 0; x < nw; x++) {
      int x0 = std::min(2 * x, w - 1) * 3, x1 = std::min(2 * x + 1, w - 1) * 3;
      for (int c = 0; c < 3; c++) o[x * 3 + c] = (uint16_t)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
    }
  }
}

template <class T, class F>
static void uploadPyramid(const T* level0, int w, int h, bool is16, F downsample) {
  if (glpTexStorage2D) {  // immutable storage: allocated once, never re-laid-out by the driver
    int levels = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) levels++;
    glpTexStorage2D(GL_TEXTURE_2D, levels, is16 ? GL_RGBA16 : GL_SRGB8_ALPHA8, w, h);
  }
  uploadLevel(0, w, h, level0, is16);
  std::vector<T> a, b;
  const T* src = level0;
  int level = 1;
  // Mip chain built on the CPU: glGenerateMipmap makes some drivers keep large scratch buffers.
  while (w > 1 || h > 1) {
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    downsample(src, w, h, b, nw, nh);
    uploadLevel(level++, nw, nh, b.data(), is16);
    a.swap(b);
    src = a.data();
    w = nw;
    h = nh;
  }
}

bool Renderer::upload(ImageData& img, std::string& err) {
  if (img.w <= 0 || img.h <= 0) { err = "empty image"; return false; }
  if (srcTex_) glDeleteTextures(1, &srcTex_);
  glGenTextures(1, &srcTex_);
  glBindTexture(GL_TEXTURE_2D, srcTex_);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (img.is16) uploadPyramid(img.px16.data(), img.w, img.h, true, downsample16);
  else uploadPyramid(img.px8.data(), img.w, img.h, false, downsample8);
  setTexParams(true);
  srcW_ = img.w;
  srcH_ = img.h;
  orient_ = img.orientation;
  img.release();  // the pixels now live only in VRAM
  if (glGetError() != 0) { err = "GPU upload failed (out of video memory?)"; return false; }
  return true;
}

void Renderer::ensureTarget(Target& t, int aw, int ah, int bw, int bh) {
  auto grow = [](int need, int have) { return need <= have ? have : ((need + 63) / 64) * 64; };
  auto make = [](unsigned& fbo, unsigned& tex, int w, int h, GLint ifmt, GLenum type) {
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, GL_RGBA, type, nullptr);
    setTexParams(false);
    if (!fbo) glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  };
  if (aw > t.aW || ah > t.aH) {
    t.aW = grow(aw, t.aW);
    t.aH = grow(ah, t.aH);
    make(t.fboA, t.texA, t.aW, t.aH, GL_RGBA16F, GL_FLOAT);
  }
  if (bw > t.bW || bh > t.bH) {
    t.bW = grow(bw, t.bW);
    t.bH = grow(bh, t.bH);
    make(t.fboB, t.texB, t.bW, t.bH, GL_RGBA8, GL_UNSIGNED_BYTE);
  }
}

void Renderer::updateCurve(const EditParams& p) {
  if (curveValid_ && sameCurves(p, curveCache_)) return;
  float lut[4][256], data[256 * 4];
  for (int c = 0; c < CURVE_COUNT; c++) buildCurveLUT(p.curve[c], lut[c]);
  for (int i = 0; i < 256; i++) {
    data[i * 4 + 0] = lut[CURVE_R][i];
    data[i * 4 + 1] = lut[CURVE_G][i];
    data[i * 4 + 2] = lut[CURVE_B][i];
    data[i * 4 + 3] = lut[CURVE_MASTER][i];
  }
  glBindTexture(GL_TEXTURE_2D, curveTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 256, 1, 0, GL_RGBA, GL_FLOAT, data);
  std::memcpy(curveCache_.curve, p.curve, sizeof(p.curve));
  curveValid_ = true;
}

void Renderer::runPasses(const EditParams& p, const Geometry& g, float o0x, float o0y, float o1x, float o1y, int w,
                         int h, int pad, float step, float fx, bool clip, float bgGrey, Target& t) {
  int aw = w + 2 * pad, ah = h + 2 * pad;
  ensureTarget(t, aw, ah, w, h);
  updateCurve(p);
  float dx = (o1x - o0x) / w, dy = (o1y - o0y) / h;

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glBindVertexArray(vao_);

  // ---- pass A: develop ----
  unsigned P = progA_;
  glUseProgram(P);
  auto U = [&](const char* n) { return glGetUniformLocation(P, n); };
  glBindFramebuffer(GL_FRAMEBUFFER, t.fboA);
  glViewport(0, 0, aw, ah);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, srcTex_);
  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, curveTex_);
  glUniform1i(U("uSrc"), 0);
  glUniform1i(U("uCurve"), 1);
  glUniform2f(U("uO0"), o0x - pad * dx, o0y - pad * dy);
  glUniform2f(U("uO1"), o1x + pad * dx, o1y + pad * dy);
  glUniformMatrix3fv(U("uM"), 1, GL_TRUE, g.outToSrc.m);
  glUniform2f(U("uSrcTexel"), 1.f / srcW_, 1.f / srcH_);
  float wb[9];
  whiteBalanceMatrix(p.temp, p.tint, wb);
  glUniformMatrix3fv(U("uWB"), 1, GL_TRUE, wb);
  glUniform1f(U("uExposure"), std::exp2(p.exposure));
  glUniform1f(U("uContrast"), p.contrast / 100.f);
  glUniform1f(U("uHighlights"), p.highlights / 100.f);
  glUniform1f(U("uShadows"), p.shadows / 100.f);
  glUniform1f(U("uWhites"), p.whites / 100.f);
  glUniform1f(U("uBlacks"), p.blacks / 100.f);
  glUniform1f(U("uClarity"), p.clarity / 100.f);
  glUniform1f(U("uTexture"), p.texture / 100.f);
  glUniform1f(U("uDehaze"), p.dehaze / 100.f);
  glUniform1f(U("uVibrance"), p.vibrance / 100.f);
  glUniform1f(U("uSaturation"), p.saturation / 100.f);
  float maxDim = (float)std::max(srcW_, srcH_);
  float baseLod = std::max(1.f, std::log2(maxDim / 40.f));
  glUniform1f(U("uBaseLod"), baseLod);
  glUniform1f(U("uTexLod"), std::clamp(std::log2(maxDim / 700.f), 0.5f, std::max(0.5f, baseLod - 1.f)));

  EditParams def;
  glUniform1i(U("uUseCurve"), sameCurves(p, def) ? 0 : 1);
  float hh[8], hs[8], hl[8];
  bool useHsl = false;
  for (int i = 0; i < kHslBands; i++) {
    hh[i] = p.hslHue[i] / 100.f;
    hs[i] = p.hslSat[i] / 100.f;
    hl[i] = p.hslLum[i] / 100.f;
    useHsl |= hh[i] != 0 || hs[i] != 0 || hl[i] != 0;
  }
  glUniform1i(U("uUseHSL"), useHsl ? 1 : 0);
  glUniform1fv(U("uHslH"), 8, hh);
  glUniform1fv(U("uHslS"), 8, hs);
  glUniform1fv(U("uHslL"), 8, hl);

  bool useGrade = false;
  const char* gradeNames[4] = {"uGradeS", "uGradeM", "uGradeH", "uGradeG"};
  float lum[4];
  for (int i = 0; i < GRADE_COUNT; i++) {
    const GradeWheel& gw = p.grade[i];
    float col[3];
    hsvToRgb(gw.hue / 360.f, 1.f, 1.f, col);
    float y = 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2];
    float k = gw.sat / 100.f * 0.15f;
    glUniform3f(U(gradeNames[i]), (col[0] - y) * k, (col[1] - y) * k, (col[2] - y) * k);
    lum[i] = gw.lum / 100.f;
    useGrade |= gw.sat != 0 || gw.lum != 0;
  }
  glUniform4f(U("uGradeLum"), lum[0], lum[1], lum[2], lum[3]);
  glUniform1f(U("uGradeCenter"), 0.5f - p.gradeBalance / 100.f * 0.25f);
  glUniform1f(U("uGradeExp"), 1.f / (0.25f + 1.5f * p.gradeBlending / 100.f));
  glUniform1i(U("uUseGrade"), useGrade ? 1 : 0);
  // AI local layers
  bool useMask = maskTex_ && anyLayer(p);
  glActiveTexture(GL_TEXTURE0 + 2);
  glBindTexture(GL_TEXTURE_2D, maskTex_ ? maskTex_ : curveTex_);
  glUniform1i(U("uMask"), 2);
  glUniform1i(U("uUseMask"), useMask ? 1 : 0);
  glUniformMatrix3fv(U("uMO"), 1, GL_TRUE, g.outToOriented.m);
  glActiveTexture(GL_TEXTURE0 + 3);
  glBindTexture(GL_TEXTURE_2D, recTex_ ? recTex_ : curveTex_);
  glUniform1i(U("uRec"), 3);
  glUniform1f(U("uRecAmt"), recTex_ ? std::clamp(p.recover / 100.f, 0.f, 1.f) : 0.f);
  float lv[8][4] = {};
  for (int i = 0; i < kMaxLayers && useMask; i++) {
    const LocalLayer& l = p.layers[i];
    if (!l.enabled) continue;
    lv[0][i] = l.exposure;
    lv[1][i] = l.contrast / 100.f;
    lv[2][i] = l.highlights / 100.f;
    lv[3][i] = l.shadows / 100.f;
    lv[4][i] = l.temp / 100.f;
    lv[5][i] = l.tint / 100.f;
    lv[6][i] = l.saturation / 100.f;
    lv[7][i] = l.clarity / 100.f;
  }
  const char* lnames[8] = {"uLExp", "uLCon", "uLHi", "uLSh", "uLTemp", "uLTint", "uLSat", "uLClar"};
  for (int k = 0; k < 8; k++) glUniform4f(U(lnames[k]), lv[k][0], lv[k][1], lv[k][2], lv[k][3]);
  glActiveTexture(GL_TEXTURE0);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // ---- pass B: finish ----
  P = progB_;
  glUseProgram(P);
  glBindFramebuffer(GL_FRAMEBUFFER, t.fboB);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, t.texA);
  glUniform1i(U("uA"), 0);
  glUniform2f(U("uO0"), o0x, o0y);
  glUniform2f(U("uO1"), o1x, o1y);
  glUniform2f(U("uATex"), (float)t.aW, (float)t.aH);
  glUniform2f(U("uAUsed"), (float)aw, (float)ah);
  glUniform2f(U("uOffset"), (float)pad, (float)pad);
  glUniform1f(U("uStep"), step);
  glUniform1f(U("uFx"), fx);
  glUniform1f(U("uSharpAmt"), p.sharpAmount / 100.f);
  glUniform1f(U("uSharpSigma"), 0.4f + p.sharpRadius * 0.4f);
  glUniform1f(U("uSharpMask"), p.sharpMasking / 100.f);
  glUniform1f(U("uNrLum"), p.nrLum / 100.f);
  glUniform1f(U("uNrCol"), p.nrColor / 100.f);
  glUniform1f(U("uVigAmt"), p.vigAmount / 100.f);
  glUniform1f(U("uVigMid"), p.vigMidpoint / 100.f);
  glUniform1f(U("uVigFeather"), p.vigFeather / 100.f);
  glUniform1f(U("uVigRound"), p.vigRoundness / 100.f);
  glUniform1f(U("uAspect"), (float)g.outW / g.outH);
  glUniform1f(U("uGrainAmt"), p.grainAmount / 100.f);
  glUniform1f(U("uGrainSize"), p.grainSize / 100.f);
  glUniform1f(U("uGrainRough"), p.grainRoughness / 100.f);
  glUniform2f(U("uOutPx"), (float)g.outW, (float)g.outH);
  glUniform1i(U("uClip"), clip ? 1 : 0);
  glUniform3f(U("uBg"), bgGrey, bgGrey, bgGrey);
  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, maskTex_ ? maskTex_ : curveTex_);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(U("uMask"), 1);
  glActiveTexture(GL_TEXTURE0 + 2);
  glBindTexture(GL_TEXTURE_2D, recTex_ ? recTex_ : curveTex_);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(U("uRec"), 2);
  glUniformMatrix3fv(U("uMO"), 1, GL_TRUE, g.outToOriented.m);
  bool showOk = maskShow_ == 4 ? recTex_ != 0 : maskTex_ != 0;
  glUniform1i(U("uMaskShow"), (showOk && &t == &view_[0]) ? maskShow_ : -1);  // preview only
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glUseProgram(0);
  glBindVertexArray(0);
}

unsigned Renderer::renderView(int slot, const EditParams& p, bool fullFrame, float o0x, float o0y, float o1x,
                              float o1y, int w, int h, float zoom, bool clip, float& uMax, float& vMax) {
  if (!srcTex_ || w <= 0 || h <= 0) return 0;
  Target& t = view_[slot & 1];
  Geometry g = computeGeometry(p, srcW_, srcH_, orient_, fullFrame);
  runPasses(p, g, o0x, o0y, o1x, o1y, w, h, 0, std::max(1.f, zoom), std::min(1.f, zoom), clip, 0.09f, t);
  uMax = (float)w / t.bW;
  vMax = (float)h / t.bH;
  return t.texB;
}

void Renderer::histogram(const EditParams& p, unsigned hist[4][256]) {
  std::memset(hist, 0, sizeof(unsigned) * 4 * 256);
  if (!srcTex_) return;
  Geometry g = computeGeometry(p, srcW_, srcH_, orient_, false);
  int w = 256, h = 256;
  if (g.outW >= g.outH) h = std::max(1, 256 * g.outH / g.outW);
  else w = std::max(1, 256 * g.outW / g.outH);
  runPasses(p, g, 0, 0, 1, 1, w, h, 0, 1.f, std::min(1.f, (float)w / g.outW), false, 0.f, hist_);
  std::vector<uint8_t> buf((size_t)w * h * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, hist_.fboB);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  for (size_t i = 0; i < buf.size(); i += 4) {
    hist[0][buf[i]]++;
    hist[1][buf[i + 1]]++;
    hist[2][buf[i + 2]]++;
    hist[3][(buf[i] * 54 + buf[i + 1] * 183 + buf[i + 2] * 19) >> 8]++;
  }
}

void Renderer::setMask(const uint8_t* rgba, int w, int h) {
  if (!maskTex_) glGenTextures(1, &maskTex_);
  glBindTexture(GL_TEXTURE_2D, maskTex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  setTexParams(false);
}

void Renderer::setRecover(const uint8_t* rgba, int w, int h) {
  if (!recTex_) glGenTextures(1, &recTex_);
  glBindTexture(GL_TEXTURE_2D, recTex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  setTexParams(false);
}

void Renderer::clearRecover() {
  if (recTex_) glDeleteTextures(1, &recTex_);
  recTex_ = 0;
}

void Renderer::clearMask() {
  if (maskTex_) glDeleteTextures(1, &maskTex_);
  maskTex_ = 0;
}

void Renderer::renderSmall(const EditParams& p, int w, int h, std::vector<uint8_t>& rgba) {
  rgba.assign((size_t)w * h * 4, 0);
  if (!srcTex_) return;
  Geometry g = computeGeometry(p, srcW_, srcH_, orient_, false);
  runPasses(p, g, 0, 0, 1, 1, w, h, 0, 1.f, std::min(1.f, (float)w / g.outW), false, 0.f, hist_);
  glBindFramebuffer(GL_FRAMEBUFFER, hist_.fboB);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::exportBegin(const EditParams& p, int outW, int outH) {
  expP_ = p;
  expW_ = outW;
  expH_ = outH;
  expTile_ = 0;
  expTilesX_ = (outW + kExportTile - 1) / kExportTile;
  expTilesY_ = (outH + kExportTile - 1) / kExportTile;
}

float Renderer::exportProgress() const {
  int total = expTilesX_ * expTilesY_;
  return total ? (float)expTile_ / total : 1.f;
}

bool Renderer::exportStep(uint8_t* rgb) {
  int total = expTilesX_ * expTilesY_;
  if (expTile_ >= total) return true;
  int tx = expTile_ % expTilesX_, ty = expTile_ / expTilesX_;
  int x0 = tx * kExportTile, y0 = ty * kExportTile;
  int w = std::min(kExportTile, expW_ - x0), h = std::min(kExportTile, expH_ - y0);
  Geometry g = computeGeometry(expP_, srcW_, srcH_, orient_, false);
  float scale = (float)expW_ / g.outW;
  runPasses(expP_, g, (float)x0 / expW_, (float)y0 / expH_, (float)(x0 + w) / expW_, (float)(y0 + h) / expH_, w, h,
            kExportPad, std::max(1.f, scale), std::min(1.f, scale), false, 0.f, exp_);
  tileBuf_.resize((size_t)w * h * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, exp_.fboB);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tileBuf_.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  for (int r = 0; r < h; r++) {
    const uint8_t* s = &tileBuf_[(size_t)r * w * 4];
    uint8_t* d = rgb + ((size_t)(y0 + r) * expW_ + x0) * 3;
    for (int c = 0; c < w; c++, s += 4, d += 3) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
  }
  expTile_++;
  if (expTile_ >= total) {
    std::vector<uint8_t>().swap(tileBuf_);
    freeTarget(exp_);  // release export VRAM right away
    return true;
  }
  return false;
}
