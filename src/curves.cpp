#include "curves.h"

#include <cmath>

static void tangents(const Curve& c, float* m) {
  int n = c.n;
  float d[kMaxCurvePts] = {};
  for (int i = 0; i < n - 1; i++) {
    float dx = c.p[i + 1].x - c.p[i].x;
    d[i] = dx > 1e-6f ? (c.p[i + 1].y - c.p[i].y) / dx : 0.f;
  }
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (int i = 1; i < n - 1; i++) m[i] = (d[i - 1] * d[i] <= 0.f) ? 0.f : 0.5f * (d[i - 1] + d[i]);
  for (int i = 0; i < n - 1; i++) {
    if (d[i] == 0.f) {
      m[i] = m[i + 1] = 0.f;
      continue;
    }
    float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
    if (s > 9.f) {
      float t = 3.f / std::sqrt(s);
      m[i] = t * a * d[i];
      m[i + 1] = t * b * d[i];
    }
  }
}

static float evalWith(const Curve& c, const float* m, float x) {
  if (x <= c.p[0].x) return c.p[0].y;
  if (x >= c.p[c.n - 1].x) return c.p[c.n - 1].y;
  int i = 0;
  while (i < c.n - 2 && x > c.p[i + 1].x) i++;
  float h = c.p[i + 1].x - c.p[i].x;
  float t = (x - c.p[i].x) / h, t2 = t * t, t3 = t2 * t;
  float y = (2 * t3 - 3 * t2 + 1) * c.p[i].y + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * c.p[i + 1].y +
            (t3 - t2) * h * m[i + 1];
  return y < 0.f ? 0.f : (y > 1.f ? 1.f : y);
}

float evalCurve(const Curve& c, float x) {
  float m[kMaxCurvePts];
  tangents(c, m);
  return evalWith(c, m, x);
}

void buildCurveLUT(const Curve& c, float out[256]) {
  float m[kMaxCurvePts];
  tangents(c, m);
  for (int i = 0; i < 256; i++) out[i] = evalWith(c, m, i / 255.f);
}

int curveInsert(Curve& c, float x, float y) {
  if (c.n >= kMaxCurvePts) return -1;
  int i = 0;
  while (i < c.n && c.p[i].x < x) i++;
  if ((i < c.n && std::fabs(c.p[i].x - x) < 0.01f) || (i > 0 && std::fabs(c.p[i - 1].x - x) < 0.01f)) return -1;
  for (int k = c.n; k > i; k--) c.p[k] = c.p[k - 1];
  c.p[i] = {x, y};
  c.n++;
  return i;
}

void curveRemove(Curve& c, int idx) {
  if (c.n <= 2 || idx < 0 || idx >= c.n) return;
  for (int k = idx; k < c.n - 1; k++) c.p[k] = c.p[k + 1];
  c.n--;
  c.p[c.n] = {0.f, 0.f};  // keep unused slots zero so memcmp comparisons stay valid
}
