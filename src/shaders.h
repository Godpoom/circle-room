// GLSL sources. Pass A: per-pixel tone & colour (linear light in, linear out).
// Pass B: neighbourhood ops (noise reduction, sharpening), vignette, grain, sRGB out.
#pragma once

static const char* kVertexShader = R"GLSL(
#version 330 core
uniform vec2 uO0;
uniform vec2 uO1;
out vec2 vO;
void main() {
  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  vec2 t = p * 0.5 + 0.5;
  vO = mix(uO0, uO1, t);   // output-image coords, y down (row 0 of the target = top)
  gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

static const char* kDevelopShader = R"GLSL(
#version 330 core
in vec2 vO;
out vec4 fragColor;

uniform sampler2D uSrc;      // linear (sRGB texture or linear 16-bit), mipmapped
uniform sampler2D uCurve;    // 256x1: r,g,b = channel curves, a = master
uniform mat3 uM;             // output coords -> source uv
uniform vec2 uSrcTexel;
uniform mat3 uWB;
uniform float uExposure;
uniform float uContrast, uHighlights, uShadows, uWhites, uBlacks;
uniform float uClarity, uTexture, uDehaze;
uniform float uVibrance, uSaturation;
uniform float uBaseLod, uTexLod;
uniform int uUseCurve, uUseHSL, uUseGrade;
uniform float uHslH[8];
uniform float uHslS[8];
uniform float uHslL[8];
uniform vec3 uGradeS, uGradeM, uGradeH, uGradeG;
uniform vec4 uGradeLum;
uniform float uGradeCenter, uGradeExp;
// AI local layers: mask channel i = layer i, sampled in EXIF-oriented image coords
uniform sampler2D uMask;
uniform int uUseMask;
uniform mat3 uMO;
// AI highlight recovery overlay (sRGB texture -> linear; alpha = blend weight)
uniform sampler2D uRec;
uniform float uRecAmt;
uniform vec4 uLExp, uLCon, uLHi, uLSh, uLTemp, uLTint, uLSat, uLClar;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

vec3 enc(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
vec3 dec(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

vec3 lodTap(vec2 uv, float lod) { return textureLod(uSrc, uv, lod).rgb; }
vec3 blurLod(vec2 uv, float lod) {
  vec2 d = uSrcTexel * exp2(lod);
  vec3 s = lodTap(uv, lod) * 4.0;
  s += (lodTap(uv + vec2(d.x, 0.0), lod) + lodTap(uv - vec2(d.x, 0.0), lod) +
        lodTap(uv + vec2(0.0, d.y), lod) + lodTap(uv - vec2(0.0, d.y), lod)) * 2.0;
  s += lodTap(uv + d, lod) + lodTap(uv - d, lod) + lodTap(uv + vec2(d.x, -d.y), lod) +
       lodTap(uv + vec2(-d.x, d.y), lod);
  return s / 16.0;
}

vec3 rgb2hsv(vec3 c) {
  vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
  vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  float e = 1.0e-10;
  return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
  vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
  vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
  return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float curveLut(float x, int ch) {
  float xc = clamp(x, 0.0, 1.0);
  float v = texture(uCurve, vec2(xc * (255.0 / 256.0) + 0.5 / 256.0, 0.5))[ch];
  return v + max(x - 1.0, 0.0);
}

float contrastCurve(float p, float amount) {
  const float pv = 0.46;  // ~18% grey in gamma 2.2
  float k = amount >= 0.0 ? 1.0 + amount * 1.3 : 1.0 / (1.0 - amount * 0.7);
  float x = clamp(p, 0.0, 1.0);
  float y = x < pv ? pv * pow(x / pv, k) : 1.0 - (1.0 - pv) * pow((1.0 - x) / (1.0 - pv), k);
  return y + max(p - 1.0, 0.0);
}

const float HC[9] = float[9](0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 270.0, 300.0, 360.0);

void main() {
  vec2 uv = (uM * vec3(vO, 1.0)).xy;
  vec2 gx = dFdx(uv), gy = dFdy(uv);
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
    fragColor = vec4(0.0);
    return;
  }
  vec3 src = textureGrad(uSrc, uv, gx, gy).rgb;
  if (uRecAmt > 0.0) {
    vec4 rc = texture(uRec, (uMO * vec3(vO, 1.0)).xy);
    src = mix(src, rc.rgb, rc.a * uRecAmt);
  }
  vec3 c = uWB * src * uExposure;
  vec4 lm = vec4(0.0);
  if (uUseMask == 1) {
    lm = texture(uMask, (uMO * vec3(vO, 1.0)).xy);
    float lt = dot(lm, uLTemp), lti = dot(lm, uLTint);
    c *= exp2(dot(lm, uLExp)) * exp2(vec3(0.35 * lt + 0.2 * lti, -0.3 * lti, -0.35 * lt + 0.2 * lti));
  }
  float lHi = dot(lm, uLHi), lSh = dot(lm, uLSh), lClar = dot(lm, uLClar);

  // Dehaze (simplified dark-channel prior with a large-scale estimate)
  if (uDehaze != 0.0) {
    vec3 hb = uWB * blurLod(uv, uBaseLod + 1.0) * uExposure;
    float dc = clamp(min(min(hb.r, hb.g), hb.b), 0.0, 1.0);
    if (uDehaze > 0.0) {
      float t = max(1.0 - uDehaze * 0.9 * dc, 0.15);
      c = max((c - vec3(1.0 - t)) / t, vec3(0.0));
    } else {
      c = mix(c, vec3(0.75 * dot(hb, LUMA) + 0.25), -uDehaze * 0.55);
    }
  }

  // Luminance-domain tone: local highlights/shadows + clarity + texture
  float L = max(dot(c, LUMA), 1.0e-6);
  float lgL = log2(L);
  float lg = lgL;
  if (uHighlights != 0.0 || uShadows != 0.0 || uClarity != 0.0 || uUseMask == 1) {
    vec3 b = uWB * blurLod(uv, uBaseLod) * uExposure;
    float Lb = max(dot(b, LUMA), 1.0e-6);
    float pm = mix(pow(L, 1.0 / 2.2), pow(Lb, 1.0 / 2.2), 0.7);
    float sm = 1.0 - smoothstep(0.0, 0.6, pm);
    float hm = smoothstep(0.35, 0.95, pm);
    lg += (uShadows + lSh) * 1.6 * sm;
    lg += (uHighlights + lHi) * 1.6 * hm;
    float mid = 1.0 - pow(abs(2.0 * clamp(pm, 0.0, 1.0) - 1.0), 2.0);
    lg += (uClarity + lClar) * 0.7 * (lgL - log2(Lb)) * mid;
  }
  if (uTexture != 0.0) {
    vec3 tb = uWB * blurLod(uv, uTexLod) * uExposure;
    float Lt = max(dot(tb, LUMA), 1.0e-6);
    lg += uTexture * 0.8 * clamp(lgL - log2(Lt), -2.0, 2.0);
  }
  float p = exp2(lg / 2.2);
  p += uBlacks * 0.12 * pow(max(1.0 - p, 0.0), 4.0);
  p += uWhites * 0.2 * pow(clamp(p, 0.0, 1.0), 4.0);
  if (uContrast != 0.0) p = contrastCurve(p, uContrast);
  float lCon = dot(lm, uLCon);
  if (lCon != 0.0) p = contrastCurve(p, lCon);
  p = max(p, 0.0);
  float Lnew = pow(p, 2.2);
  c *= Lnew / L;
  // Roll off saturated highlights instead of hard per-channel clipping
  float mx = max(max(c.r, c.g), c.b);
  if (mx > 1.0) c = mix(c, vec3(max(dot(c, LUMA), 1.0)), smoothstep(1.0, 3.0, mx));

  vec3 e = enc(c);

  if (uUseCurve == 1) {
    e = vec3(curveLut(e.r, 3), curveLut(e.g, 3), curveLut(e.b, 3));
    e = vec3(curveLut(e.r, 0), curveLut(e.g, 1), curveLut(e.b, 2));
  }

  if (uUseHSL == 1) {
    vec3 hsv = rgb2hsv(e);
    float hd = hsv.x * 360.0;
    int k = 0;
    for (int i = 0; i < 8; i++) if (hd >= HC[i]) k = i;
    float t = clamp((hd - HC[k]) / (HC[k + 1] - HC[k]), 0.0, 1.0);
    t = t * t * (3.0 - 2.0 * t);
    int k2 = (k + 1) % 8;
    float dh = mix(uHslH[k], uHslH[k2], t);
    float ds = mix(uHslS[k], uHslS[k2], t);
    float dl = mix(uHslL[k], uHslL[k2], t);
    float w = smoothstep(0.0, 0.25, hsv.y);
    float Y0 = dot(e, LUMA);
    if (dh != 0.0) {  // rotate hue, then restore the original luminance
      hsv.x = fract(hsv.x + dh * w * (30.0 / 360.0) + 1.0);
      vec3 r = hsv2rgb(hsv);
      e = r * (Y0 / max(dot(r, LUMA), 1.0e-5));
    }
    // Saturation scales chroma around luminance (blue -> dark grey, not white like HSV would)
    e = max(vec3(Y0) + (e - vec3(Y0)) * max(1.0 + ds * w, 0.0), vec3(0.0));
    e *= max(1.0 + dl * 0.5 * w, 0.0);
  }

  if (uUseGrade == 1) {
    float pl = clamp(dot(e, LUMA), 0.0, 1.0);
    float ws = pow(1.0 - smoothstep(0.0, uGradeCenter, pl), uGradeExp);
    float wh = pow(smoothstep(uGradeCenter, 1.0, pl), uGradeExp);
    float wm = max(1.0 - ws - wh, 0.0);
    e += uGradeS * ws + uGradeM * wm + uGradeH * wh + uGradeG;
    e *= max(1.0 + 0.35 * (uGradeLum.x * ws + uGradeLum.y * wm + uGradeLum.z * wh + uGradeLum.w), 0.0);
  }

  float lSat = dot(lm, uLSat);
  if (uVibrance != 0.0 || uSaturation != 0.0 || lSat != 0.0) {
    float Y = dot(e, LUMA);
    float emx = max(max(e.r, e.g), e.b), emn = min(min(e.r, e.g), e.b);
    float s = clamp((emx - emn) / max(emx, 1.0e-5), 0.0, 1.0);
    float hue = rgb2hsv(max(e, vec3(0.0))).x * 360.0;
    float skin = 1.0 - smoothstep(10.0, 25.0, abs(hue - 30.0));
    float vib = uVibrance * (1.0 - s) * (uVibrance > 0.0 ? 1.0 - 0.5 * skin : 1.0);
    float f = max((1.0 + uSaturation) * (1.0 + vib) * (1.0 + lSat), 0.0);
    e = vec3(Y) + (e - vec3(Y)) * f;
  }

  fragColor = vec4(dec(e), 1.0);
}
)GLSL";

static const char* kFinishShader = R"GLSL(
#version 330 core
in vec2 vO;
out vec4 fragColor;

uniform sampler2D uA;
uniform vec2 uATex;      // allocated size of A
uniform vec2 uAUsed;     // used size of A
uniform vec2 uOffset;    // padding of A around this target
uniform float uStep;     // A pixels per image pixel (>= 1)
uniform float uFx;       // strength scale for zoomed-out previews
uniform float uSharpAmt, uSharpSigma, uSharpMask;
uniform float uNrLum, uNrCol;
uniform float uVigAmt, uVigMid, uVigFeather, uVigRound, uAspect;
uniform float uGrainAmt, uGrainSize, uGrainRough;
uniform vec2 uOutPx;
uniform int uClip;
uniform sampler2D uMask;
uniform mat3 uMO;
uniform int uMaskShow;  // -1 off, 0..3 layer channel, 4 = recovered highlights
uniform sampler2D uRec;
uniform vec3 uBg;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
vec2 pc;
vec3 centerLin;

vec3 enc(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
vec3 dec(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 S(vec2 off) {
  vec2 p = clamp(pc + off * uStep, vec2(0.5), uAUsed - 0.5);
  vec4 v = texture(uA, p / uATex);
  return v.a < 0.5 ? centerLin : v.rgb;  // never pull in out-of-image pixels
}
float hash(vec2 p) {
  uvec2 q = uvec2(ivec2(floor(p)) + 32768);
  uint h = (q.x * 1597334677u) ^ (q.y * 3812015801u);
  h = h * 1103515245u + 12345u;
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
  return float(h) * (1.0 / 4294967295.0);
}
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = hash(i), b = hash(i + vec2(1, 0)), c = hash(i + vec2(0, 1)), d = hash(i + vec2(1, 1));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec3 toSrgb(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
  pc = gl_FragCoord.xy + uOffset;
  vec4 c0 = texture(uA, pc / uATex);
  if (c0.a < 0.5) { fragColor = vec4(uBg, 1.0); return; }
  centerLin = c0.rgb;
  vec3 e0 = enc(c0.rgb);
  vec3 e = e0;
  float nrL = uNrLum * uFx, nrC = uNrCol * uFx, sharp = uSharpAmt * uFx;

  if (nrL > 0.001 || nrC > 0.001 || sharp > 0.001) {
    float Y0 = dot(e0, LUMA);
    float sr = 0.015 + nrL * 0.12;
    float srInv = 1.0 / (2.0 * sr * sr);
    float ssInv = 1.0 / (2.0 * uSharpSigma * uSharpSigma);
    vec3 sumC = vec3(0.0);
    float sumB = 0.0, wB = 0.0, wC = 0.0, sumG = 0.0, wG = 0.0, gxs = 0.0, gys = 0.0;
    for (int j = -2; j <= 2; j++) {
      for (int i = -2; i <= 2; i++) {
        vec3 en = enc(S(vec2(i, j)));
        float Y = dot(en, LUMA);
        float d2 = float(i * i + j * j);
        float ws = exp(-d2 / 4.5);
        float dy = Y - Y0;
        float wr = exp(-dy * dy * srInv);
        sumB += Y * ws * wr; wB += ws * wr;
        float wc = ws * exp(-dy * dy * srInv * 0.25);
        sumC += (en - vec3(Y)) * wc; wC += wc;
        float wg = exp(-d2 * ssInv);
        sumG += Y * wg; wG += wg;
        gxs += Y * float(i) * ws; gys += Y * float(j) * ws;
      }
    }
    float Yn = mix(Y0, sumB / wB, clamp(nrL, 0.0, 1.0));
    vec3 chroma = mix(e0 - vec3(Y0), sumC / wC, clamp(nrC, 0.0, 1.0));
    if (sharp > 0.001) {
      float detail = Y0 - sumG / wG;
      float mask = 1.0;
      if (uSharpMask > 0.0) {
        float g = length(vec2(gxs, gys)) * 0.25;
        mask = smoothstep(uSharpMask * 0.06, uSharpMask * 0.06 + 0.03, g);
      }
      Yn += detail * sharp * 1.2 * mask * (1.0 - 0.6 * clamp(nrL, 0.0, 1.0));
    }
    e = vec3(Yn) + chroma;
  }

  if (uVigAmt != 0.0) {
    vec2 q = (vO - 0.5) * 2.0;
    if (uVigRound > 0.0) {
      if (uAspect > 1.0) q.x *= mix(1.0, uAspect, uVigRound);
      else q.y *= mix(1.0, 1.0 / uAspect, uVigRound);
    }
    float n = mix(2.0, 6.0, max(-uVigRound, 0.0));
    float d = pow(pow(abs(q.x), n) + pow(abs(q.y), n), 1.0 / n) / pow(2.0, 1.0 / n);
    float start = uVigMid * 0.9;
    float f = smoothstep(start, start + 0.05 + uVigFeather * 0.95, d);
    e *= exp2(uVigAmt * 1.5 * f / 2.2);
  }

  if (uGrainAmt > 0.0) {
    vec2 ip = vO * uOutPx / (0.6 + uGrainSize * 3.0);
    float g = mix(vnoise(ip), vnoise(ip * 2.37 + 17.0), uGrainRough * 0.6) - 0.5;
    float Y = clamp(dot(e, LUMA), 0.0, 1.0);
    e += g * uGrainAmt * 0.2 * (1.0 - abs(2.0 * Y - 1.0) * 0.5);
  }

  vec3 lin = dec(e);
  vec3 o = toSrgb(lin);
  if (uClip == 1) {
    if (max(max(lin.r, lin.g), lin.b) >= 0.999) o = vec3(1.0, 0.1, 0.1);
    else if (max(max(lin.r, lin.g), lin.b) <= 0.0005) o = vec3(0.1, 0.35, 1.0);
  }
  if (uMaskShow >= 0) {
    vec2 mo = (uMO * vec3(vO, 1.0)).xy;
    float m = uMaskShow == 4 ? texture(uRec, mo).a : texture(uMask, mo)[uMaskShow];
    o = mix(o, vec3(1.0, 0.15, 0.15), 0.55 * m);
  }
  o += (hash(gl_FragCoord.xy + 7.0) - 0.5) / 255.0;  // dither against banding
  fragColor = vec4(o, 1.0);
}
)GLSL";

// Photo layers: composites up to four layers onto the source, in source pixels, before develop.
// Blending happens on sRGB-encoded values like Photoshop; untouched pixels pass through exactly.
static const char* kComposeShader = R"GLSL(
#version 330 core
in vec2 vO;
out vec4 fragColor;

uniform sampler2D uSrc;
uniform mat3 uSO;                       // source uv -> oriented uv
uniform int uN;
uniform sampler2D uC0, uC1, uC2, uC3;   // layer colour (sRGB textures, sampled linear)
uniform sampler2D uK0, uK1, uK2, uK3;   // layer alpha (R8)
uniform mat3 uL0, uL1, uL2, uL3;        // oriented uv -> layer uv
uniform vec4 uOp, uExp, uTemp, uSat, uBlend, uHasK;

vec3 toS(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
vec3 toL(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 blendOp(vec3 b, vec3 l, int m) {
  if (m == 1) return b * l;
  if (m == 2) return 1.0 - (1.0 - b) * (1.0 - l);
  if (m == 3) return mix(2.0 * b * l, 1.0 - 2.0 * (1.0 - b) * (1.0 - l), step(0.5, b));
  if (m == 4) {
    vec3 d = mix(((16.0 * b - 12.0) * b + 4.0) * b, sqrt(b), step(0.25, b));
    return mix(b - (1.0 - 2.0 * l) * b * (1.0 - b), b + (2.0 * l - 1.0) * (d - b), step(0.5, l));
  }
  if (m == 5) return min(b, l);
  if (m == 6) return max(b, l);
  return l;
}

vec3 over(vec3 base, sampler2D C, sampler2D K, mat3 L, int i, vec2 ou) {
  vec2 lu = (L * vec3(ou, 1.0)).xy;
  vec2 fw = max(fwidth(lu), vec2(1.0e-7));
  vec2 e = min(lu, 1.0 - lu) / fw;
  float edge = clamp(min(e.x, e.y) + 0.5, 0.0, 1.0);  // antialiased layer border
  vec2 cu = clamp(lu, 0.0, 1.0);
  vec3 c = texture(C, cu).rgb * uExp[i];
  float t = uTemp[i];
  c *= vec3(1.0 + 0.25 * t, 1.0 + 0.02 * t, 1.0 - 0.25 * t);
  float Y = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c = max(vec3(Y) + (c - vec3(Y)) * (1.0 + uSat[i]), vec3(0.0));
  float a = uOp[i] * edge * (uHasK[i] > 0.5 ? texture(K, cu).r : 1.0);
  if (a <= 0.0) return base;
  vec3 bs = toS(base);
  return toL(mix(bs, blendOp(bs, toS(c), int(uBlend[i] + 0.5)), a));
}

void main() {
  vec3 c = textureLod(uSrc, vO, 0.0).rgb;
  vec2 ou = (uSO * vec3(vO, 1.0)).xy;
  if (uN > 0) c = over(c, uC0, uK0, uL0, 0, ou);
  if (uN > 1) c = over(c, uC1, uK1, uL1, 1, ou);
  if (uN > 2) c = over(c, uC2, uK2, uL2, 2, ou);
  if (uN > 3) c = over(c, uC3, uK3, uL3, 3, ou);
  fragColor = vec4(c, 1.0);
}
)GLSL";

// Magic-eraser patch: drawn with blending over a copy of the source (scissored to its rect).
static const char* kPatchShader = R"GLSL(
#version 330 core
in vec2 vO;
out vec4 fragColor;
uniform sampler2D uP;
uniform mat3 uSO;   // source uv -> oriented uv
uniform vec4 uR;    // patch rect, oriented uv: x0, y0, x1, y1
void main() {
  vec2 t = ((uSO * vec3(vO, 1.0)).xy - uR.xy) / (uR.zw - uR.xy);
  if (t.x < 0.0 || t.y < 0.0 || t.x > 1.0 || t.y > 1.0) discard;
  fragColor = texture(uP, t);
}
)GLSL";
