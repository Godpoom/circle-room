#pragma once
#include "params.h"

// Monotone cubic (Fritsch-Carlson) interpolation through the curve points.
float evalCurve(const Curve& c, float x);
void buildCurveLUT(const Curve& c, float out[256]);
// Inserts a point keeping x order; returns its index or -1 when full.
int curveInsert(Curve& c, float x, float y);
void curveRemove(Curve& c, int idx);
