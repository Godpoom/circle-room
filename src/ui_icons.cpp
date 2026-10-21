#include "ui_icons.h"

#include <cmath>

#include "imgui_internal.h"
#include "ui_panels.h"

static const float kPi = 3.14159265f;

static ImVec2 P(ImVec2 c, float r, float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); }

static void arrowHead(ImDrawList* dl, ImVec2 tip, float dx, float dy, float len, ImU32 col) {
  // Filled triangle pointing along (dx, dy).
  float nx = -dy, ny = dx;
  ImVec2 b(tip.x - dx * len, tip.y - dy * len);
  dl->AddTriangleFilled(tip, ImVec2(b.x + nx * len * 0.6f, b.y + ny * len * 0.6f),
                        ImVec2(b.x - nx * len * 0.6f, b.y - ny * len * 0.6f), col);
}

void drawIcon(ImDrawList* dl, int icon, ImVec2 c, float size, ImU32 col) {
  float r = size * 0.5f;
  float t = std::max(1.4f, size * 0.085f);
  switch (icon) {
    case IC_AI: {  // two sparkles
      auto star = [&](ImVec2 o, float k) {
        ImVec2 pts[8];
        for (int i = 0; i < 8; i++) {
          float a = i * kPi / 4 - kPi / 2, d = (i & 1) ? 0.26f * k : k;
          pts[i] = ImVec2(o.x + std::cos(a) * d * r, o.y + std::sin(a) * d * r);
        }
        dl->AddConvexPolyFilled(pts, 8, col);
      };
      star(P(c, r, -0.18f, 0.12f), 0.78f);
      star(P(c, r, 0.55f, -0.52f), 0.36f);
      break;
    }
    case IC_LIGHT: {  // sun
      dl->AddCircle(c, r * 0.36f, col, 24, t);
      for (int i = 0; i < 8; i++) {
        float a = i * kPi / 4;
        dl->AddLine(P(c, r, std::cos(a) * 0.6f, std::sin(a) * 0.6f), P(c, r, std::cos(a) * 0.88f, std::sin(a) * 0.88f), col, t);
      }
      break;
    }
    case IC_COLOR: {  // three overlapping drops of colour
      for (int i = 0; i < 3; i++) {
        float a = -kPi / 2 + i * 2 * kPi / 3;
        dl->AddCircle(P(c, r, std::cos(a) * 0.32f, std::sin(a) * 0.32f), r * 0.46f, col, 24, t);
      }
      break;
    }
    case IC_DETAIL: {  // diamond with a centre point
      ImVec2 q[4] = {P(c, r, 0, -0.85f), P(c, r, 0.85f, 0), P(c, r, 0, 0.85f), P(c, r, -0.85f, 0)};
      dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t);
      dl->AddCircleFilled(c, r * 0.18f, col);
      break;
    }
    case IC_EFFECTS: {  // vignette: frame with a soft centre
      dl->AddRect(P(c, r, -0.85f, -0.7f), P(c, r, 0.85f, 0.7f), col, r * 0.2f, 0, t);
      dl->AddCircleFilled(c, r * 0.36f, col);
      break;
    }
    case IC_LOCAL: {  // person inside a dashed selection
      dl->AddCircle(P(c, r, 0, -0.32f), r * 0.24f, col, 20, t);
      dl->PathArcTo(P(c, r, 0, 0.62f), r * 0.5f, kPi * 1.05f, kPi * 1.95f, 16);
      dl->PathStroke(col, 0, t);
      for (int i = 0; i < 4; i++) {
        float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
        ImVec2 k = P(c, r, sx * 0.9f, sy * 0.9f);
        dl->AddLine(k, ImVec2(k.x - sx * r * 0.32f, k.y), col, t);
        dl->AddLine(k, ImVec2(k.x, k.y - sy * r * 0.32f), col, t);
      }
      break;
    }
    case IC_CROP: {
      ImVec2 a[3] = {P(c, r, -0.55f, -0.95f), P(c, r, -0.55f, 0.55f), P(c, r, 0.95f, 0.55f)};
      ImVec2 b[3] = {P(c, r, -0.95f, -0.55f), P(c, r, 0.55f, -0.55f), P(c, r, 0.55f, 0.95f)};
      dl->AddPolyline(a, 3, col, 0, t);
      dl->AddPolyline(b, 3, col, 0, t);
      break;
    }
    case IC_LOOKS: {  // stacked cards
      dl->AddRect(P(c, r, -0.45f, -0.85f), P(c, r, 0.85f, 0.35f), col, r * 0.18f, 0, t);
      dl->AddRectFilled(P(c, r, -0.85f, -0.4f), P(c, r, 0.45f, 0.85f), col, r * 0.18f);
      break;
    }
    case IC_HISTORY: {  // clock
      dl->AddCircle(c, r * 0.82f, col, 28, t);
      dl->AddLine(c, P(c, r, 0, -0.5f), col, t);
      dl->AddLine(c, P(c, r, 0.38f, 0.2f), col, t);
      break;
    }
    case IC_UNDO:
    case IC_REDO: {  // curved arrow; redo is the mirror image
      float s = icon == IC_UNDO ? 1.f : -1.f;
      ImVec2 ac = P(c, r, 0, 0.15f);
      dl->PathLineTo(P(c, r, 0.6f * s, 0.6f));
      if (s > 0) dl->PathArcTo(ac, r * 0.6f, 2 * kPi, kPi, 18);
      else dl->PathArcTo(ac, r * 0.6f, kPi, 2 * kPi, 18);
      dl->PathStroke(col, 0, t);
      arrowHead(dl, P(c, r, -0.6f * s, 0.55f), 0, 1, r * 0.42f, col);
      break;
    }
    case IC_RESET: {
      dl->PathArcTo(c, r * 0.68f, kPi * 0.75f, kPi * 2.45f, 24);
      dl->PathStroke(col, 0, t);
      ImVec2 e(c.x + std::cos(kPi * 0.75f) * r * 0.68f, c.y + std::sin(kPi * 0.75f) * r * 0.68f);
      arrowHead(dl, ImVec2(e.x - r * 0.02f, e.y + r * 0.08f), 0.55f, 0.83f, r * 0.42f, col);
      break;
    }
    case IC_OPEN: {  // folder
      ImVec2 q[6] = {P(c, r, -0.9f, -0.6f), P(c, r, -0.3f, -0.6f), P(c, r, -0.1f, -0.35f),
                     P(c, r, 0.9f, -0.35f), P(c, r, 0.9f, 0.7f),  P(c, r, -0.9f, 0.7f)};
      dl->AddPolyline(q, 6, col, ImDrawFlags_Closed, t);
      break;
    }
    case IC_EXPORT: {  // arrow leaving a tray
      ImVec2 q[4] = {P(c, r, -0.85f, 0.05f), P(c, r, -0.85f, 0.8f), P(c, r, 0.85f, 0.8f), P(c, r, 0.85f, 0.05f)};
      dl->AddPolyline(q, 4, col, 0, t);
      dl->AddLine(P(c, r, 0, 0.35f), P(c, r, 0, -0.5f), col, t);
      arrowHead(dl, P(c, r, 0, -0.92f), 0, -1, r * 0.45f, col);
      break;
    }
    case IC_COMPARE: {  // half original, half edited
      dl->AddRect(P(c, r, -0.9f, -0.7f), P(c, r, 0.9f, 0.7f), col, r * 0.15f, 0, t);
      dl->AddRectFilled(P(c, r, -0.9f, -0.7f), P(c, r, 0.f, 0.7f), col, r * 0.15f, ImDrawFlags_RoundCornersLeft);
      break;
    }
    case IC_SPLIT: {
      dl->AddRect(P(c, r, -0.9f, -0.7f), P(c, r, 0.9f, 0.7f), col, r * 0.15f, 0, t);
      dl->AddLine(P(c, r, 0, -0.95f), P(c, r, 0, 0.95f), col, t);
      dl->AddCircleFilled(c, r * 0.2f, col);
      break;
    }
    case IC_FIT: {  // four outward corners
      for (int i = 0; i < 4; i++) {
        float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
        ImVec2 k = P(c, r, sx * 0.85f, sy * 0.75f);
        dl->AddLine(k, ImVec2(k.x - sx * r * 0.45f, k.y), col, t);
        dl->AddLine(k, ImVec2(k.x, k.y - sy * r * 0.45f), col, t);
      }
      break;
    }
    case IC_ZOOM100: {
      const char* s = "1:1";
      ImVec2 ts = ImGui::CalcTextSize(s);
      dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, s);
      break;
    }
    case IC_CLIP: {  // warning triangle
      ImVec2 q[3] = {P(c, r, 0, -0.85f), P(c, r, 0.9f, 0.75f), P(c, r, -0.9f, 0.75f)};
      dl->AddPolyline(q, 3, col, ImDrawFlags_Closed, t);
      dl->AddLine(P(c, r, 0, -0.3f), P(c, r, 0, 0.2f), col, t);
      dl->AddCircleFilled(P(c, r, 0, 0.45f), t * 0.7f, col);
      break;
    }
    case IC_HISTO: {  // little mountain range
      ImVec2 q[7] = {P(c, r, -0.9f, 0.7f), P(c, r, -0.5f, -0.1f), P(c, r, -0.2f, 0.3f), P(c, r, 0.2f, -0.7f),
                     P(c, r, 0.55f, 0.1f), P(c, r, 0.9f, -0.2f), P(c, r, 0.9f, 0.7f)};
      dl->AddPolyline(q, 7, col, ImDrawFlags_Closed, t);
      break;
    }
    case IC_EDIT: {  // three sliders
      for (int i = 0; i < 3; i++) {
        float y = -0.6f + i * 0.6f, k = i == 1 ? 0.35f : (i == 0 ? -0.35f : 0.1f);
        dl->AddLine(P(c, r, -0.85f, y), P(c, r, 0.85f, y), col, t);
        dl->AddCircleFilled(P(c, r, k, y), r * 0.2f, col);
      }
      break;
    }
    case IC_GRID: {
      for (int i = 0; i < 4; i++) {
        float x = (i & 1) ? 0.1f : -0.85f, y = (i & 2) ? 0.1f : -0.85f;
        dl->AddRectFilled(P(c, r, x, y), P(c, r, x + 0.75f, y + 0.75f), col, r * 0.12f);
      }
      break;
    }
    case IC_LOGO: {  // the Circle Room mark: a paper ring with a safelight-red disc resting low inside, on a graphite tile
      float R = r;
      dl->AddRectFilled(P(c, R, -1, -1), P(c, R, 1, 1), pal::kRail, R * 0.44f);
      dl->AddRect(P(c, R, -1, -1), P(c, R, 1, 1), pal::kFrame, R * 0.44f, 0, 1.f);
      dl->AddCircleFilled(P(c, R, 0.03f, 0.12f), R * 0.34f, pal::kAccent, 28);
      dl->AddCircleFilled(P(c, R, -0.06f, -0.02f), R * 0.1f, IM_COL32(255, 150, 110, 170), 16);
      dl->AddCircle(c, R * 0.545f, pal::kText, 36, std::max(1.5f, R * 0.15f));
      break;
    }
  }
}

bool iconButton(const char* id, int icon, bool selected, const char* tip, float size) {
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  bool hov = ImGui::IsItemHovered(), dis = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p0.x + size, p0.y + size);
  if (selected) dl->AddRectFilled(p0, p1, pal::kAccentSoft, size * 0.25f);
  else if (hov && !dis) dl->AddRectFilled(p0, p1, pal::kHover, size * 0.25f);
  ImU32 col = dis ? pal::kFaint : (selected ? pal::kAccent : (hov ? pal::kText : pal::kMuted));
  drawIcon(dl, icon, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), size * 0.5f, col);
  if (tip && hov && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", tip);
  return pressed;
}

bool railButton(const char* id, int icon, const char* label, bool selected, float w, float h) {
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
  bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float pad = w * 0.1f;
  ImVec2 a(p0.x + pad, p0.y + 2), b(p0.x + w - pad, p0.y + h - 2);
  if (selected) {
    dl->AddRectFilled(a, b, pal::kAccentSoft, w * 0.16f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + h * 0.25f), ImVec2(p0.x + 3, p0.y + h * 0.75f), pal::kAccent, 2.f);
  } else if (hov) {
    dl->AddRectFilled(a, b, pal::kHover, w * 0.16f);
  }
  ImU32 col = selected ? pal::kAccent : (hov ? pal::kText : pal::kMuted);
  float is = h * 0.34f;
  drawIcon(dl, icon, ImVec2(p0.x + w * 0.5f, p0.y + h * 0.36f), is, col);
  if (g_fontSmall) ImGui::PushFont(g_fontSmall);
  ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + h * 0.66f), selected ? pal::kText : col, label);
  if (g_fontSmall) ImGui::PopFont();
  return pressed;
}
