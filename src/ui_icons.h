// Vector icons drawn with ImDrawList primitives: no icon font or image files, a few
// dozen vertices each, crisp at any DPI.
#pragma once
#include "imgui.h"

enum Icon {
  IC_AI, IC_LIGHT, IC_COLOR, IC_DETAIL, IC_EFFECTS, IC_LOCAL, IC_CROP, IC_LOOKS, IC_HISTORY,
  IC_UNDO, IC_REDO, IC_RESET, IC_OPEN, IC_EXPORT, IC_COMPARE, IC_SPLIT, IC_FIT, IC_ZOOM100,
  IC_CLIP, IC_HISTO, IC_EDIT, IC_GRID, IC_LOGO, IC_LAYERS, IC_MOVE, IC_WAND, IC_ERASER, IC_BRUSH, IC_PLUS,
};

void drawIcon(ImDrawList* dl, int icon, ImVec2 center, float size, ImU32 col);
// Square icon button; `selected` paints the accent state. Tooltip shown on hover.
bool iconButton(const char* id, int icon, bool selected, const char* tip, float size);
// Vertical rail entry: icon with a small caption below.
bool railButton(const char* id, int icon, const char* label, bool selected, float w, float h);
