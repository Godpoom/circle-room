#include "ui_panels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "curves.h"
#include "i18n.h"
#include "imgui_internal.h"
#include "ui_icons.h"

ImFont* g_fontBold = nullptr;
ImFont* g_fontSmall = nullptr;
ImFont* g_fontMono = nullptr;
ImFont* g_fontTitle = nullptr;

static float uiScale() { return ImGui::GetFontSize() / 15.f; }
static ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
static ImU32 withAlpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | ((ImU32)a << 24); }
static void pushFont(ImFont* f) { ImGui::PushFont(f ? f : ImGui::GetFont()); }

void applyTheme(float scale) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark(&st);
  // Nearly square corners and 1 px rules: closer to printed camera markings than to app chrome.
  st.WindowRounding = 3.f;
  st.ChildRounding = 0.f;
  st.FrameRounding = 2.f;
  st.GrabRounding = 1.f;
  st.PopupRounding = 3.f;
  st.ScrollbarRounding = 0.f;
  st.TabRounding = 2.f;
  st.GrabMinSize = 10.f;
  st.FramePadding = ImVec2(10.f, 5.f);
  st.ItemSpacing = ImVec2(8.f, 6.f);
  st.ItemInnerSpacing = ImVec2(6.f, 4.f);
  st.WindowPadding = ImVec2(14.f, 12.f);
  st.ScrollbarSize = 5.f;
  st.WindowBorderSize = 1.f;
  st.ChildBorderSize = 0.f;
  st.PopupBorderSize = 1.f;
  st.FrameBorderSize = 1.f;
  st.WindowTitleAlign = ImVec2(0.f, 0.5f);
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = v4(pal::kPanel);
  c[ImGuiCol_ChildBg] = v4(pal::kPanel);
  c[ImGuiCol_PopupBg] = v4(pal::kCard);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = v4(pal::kCard);
  c[ImGuiCol_Text] = v4(pal::kText);
  c[ImGuiCol_TextDisabled] = v4(pal::kMuted);
  c[ImGuiCol_Border] = v4(pal::kFrame);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = v4(pal::kCanvas);
  c[ImGuiCol_FrameBgHovered] = v4(pal::kHover);
  c[ImGuiCol_FrameBgActive] = v4(pal::kHover);
  c[ImGuiCol_SliderGrab] = v4(pal::kText);
  c[ImGuiCol_SliderGrabActive] = v4(pal::kAccent);
  c[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ButtonHovered] = v4(pal::kHover);
  c[ImGuiCol_ButtonActive] = v4(pal::kFrame);
  c[ImGuiCol_Header] = v4(pal::kHover);
  c[ImGuiCol_HeaderHovered] = v4(pal::kHover);
  c[ImGuiCol_HeaderActive] = v4(pal::kFrame);
  c[ImGuiCol_CheckMark] = v4(pal::kAccent);
  c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = v4(pal::kFrame);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = v4(pal::kFrame);
  c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = v4(pal::kTrack);
  c[ImGuiCol_PlotHistogram] = v4(pal::kText);
  c[ImGuiCol_TextSelectedBg] = v4(withAlpha(pal::kAccent, 80));
  c[ImGuiCol_NavCursor] = v4(pal::kAccent);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.6f);
  st.ScaleAllSizes(scale);
}

// Outlined tag, like a label on a negative sleeve.
bool uiPill(const char* label, bool selected, ImU32 textCol) {
  float s = uiScale();
  ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  ImVec2 size(ts.x + 16.f * s, ts.y + 8.f * s);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  bool r = ImGui::InvisibleButton(label, size);
  bool hov = ImGui::IsItemHovered(), dis = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p0.x + size.x, p0.y + size.y);
  if (selected) dl->AddRectFilled(p0, p1, pal::kFrame, 2.f * s);
  else if (hov && !dis) dl->AddRectFilled(p0, p1, pal::kHover, 2.f * s);
  dl->AddRect(p0, p1, selected || hov ? pal::kTrack : pal::kFrame, 2.f * s, 0, 1.f);
  if (selected) dl->AddRectFilled(ImVec2(p0.x, p0.y + 3.f * s), ImVec2(p0.x + 2.f * s, p1.y - 3.f * s), pal::kAccent);
  ImU32 col = dis ? pal::kFaint : (textCol ? textCol : (selected ? pal::kText : pal::kMuted));
  dl->AddText(ImVec2(p0.x + 8.f * s, p0.y + 4.f * s), col, label, ImGui::FindRenderedTextEnd(label));
  return r;
}

// Paper-coloured button with ink text: the one action that matters on a page.
bool uiPrimaryButton(const char* label, ImVec2 size) {
  ImGui::PushStyleColor(ImGuiCol_Button, pal::kText);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(250, 246, 238, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(214, 207, 195, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kInk);
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
  bool r = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  return r;
}

// Text tabs with an underline, like an index; no pill backgrounds.
int uiSegmented(const char* id, const char* const* labels, int n, int sel, bool fill) {
  float s = uiScale();
  ImGui::PushID(id);
  float h = ImGui::GetFrameHeight();
  float gap = 18.f * s;
  n = std::min(n, 16);
  float widths[16], total = 0;
  for (int i = 0; i < n; i++) total += widths[i] = ImGui::CalcTextSize(labels[i]).x;
  float avail = ImGui::GetContentRegionAvail().x;
  if (fill && n > 1) gap = std::max(gap, (avail - total) / (n - 1) * 0.5f);
  gap = std::min(gap, 34.f * s);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float x = p0.x, full = total + gap * (n - 1);
  if (fill) dl->AddLine(ImVec2(p0.x, p0.y + h - 0.5f), ImVec2(p0.x + avail, p0.y + h - 0.5f), pal::kFrame, 1.f);
  int clicked = -1;
  for (int i = 0; i < n; i++) {
    float w = widths[i];
    ImGui::SetCursorScreenPos(ImVec2(x - gap * 0.5f * (i > 0), p0.y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##tab", ImVec2(w + gap * 0.5f * ((i > 0) + (i < n - 1)), h))) clicked = i;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImVec2 ts = ImGui::CalcTextSize(labels[i]);
    dl->AddText(ImVec2(x, p0.y + (h - ts.y) * 0.5f - 1.f * s), i == sel ? pal::kText : (hov ? pal::kText : pal::kMuted),
                labels[i]);
    if (i == sel) dl->AddRectFilled(ImVec2(x, p0.y + h - 2.f * s), ImVec2(x + w, p0.y + h), pal::kAccent);
    x += w + gap;
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(fill ? avail : full, h));
  ImGui::PopID();
  return clicked;
}

bool uiTextToggle(const char* label, bool on, const char* tip) {
  float s = uiScale();
  ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  ImVec2 size(ts.x + 14.f * s, ImGui::GetFrameHeight());
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  bool r = ImGui::InvisibleButton(label, size);
  bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float tx = p0.x + 7.f * s, ty = p0.y + (size.y - ts.y) * 0.5f - 1.f * s;
  dl->AddText(ImVec2(tx, ty), on ? pal::kText : (hov ? pal::kText : pal::kMuted), label, ImGui::FindRenderedTextEnd(label));
  if (on) dl->AddRectFilled(ImVec2(tx, p0.y + size.y - 3.f * s), ImVec2(tx + ts.x, p0.y + size.y - 1.f * s), pal::kAccent);
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
  return r;
}

void uiHint(const char* text) {
  pushFont(g_fontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

static int g_sectionNo = 0;
void uiSectionReset() { g_sectionNo = 0; }

bool uiCardBegin(const char* id, const char* title, bool resettable) {
  float s = uiScale();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar();
  if (!title) {
    ImGui::Dummy(ImVec2(0, 2.f * s));
    return false;
  }
  float w = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  dl->AddLine(ImVec2(p0.x, p0.y + 0.5f), ImVec2(p0.x + w, p0.y + 0.5f), pal::kFrame, 1.f);
  ImGui::Dummy(ImVec2(0, 9.f * s));
  // "01" index in the DIN face, then the title.
  char num[8];
  snprintf(num, sizeof(num), "%02d", ++g_sectionNo);
  ImVec2 tp = ImGui::GetCursorScreenPos();
  pushFont(g_fontMono);
  ImVec2 ns = ImGui::CalcTextSize(num);
  float lh = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  pushFont(g_fontBold);
  float th = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  pushFont(g_fontMono);
  dl->AddText(ImVec2(tp.x, tp.y + (th - lh) * 0.5f + 1.f * s), pal::kAccent, num);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(tp.x + ns.x + 10.f * s, tp.y));
  pushFont(g_fontBold);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  bool reset = false;
  if (resettable) {
    const char* lbl = tr(T_ResetShort);
    pushFont(g_fontSmall);
    ImVec2 ls = ImGui::CalcTextSize(lbl);
    ImVec2 rp(p0.x + w - ls.x, tp.y + (th - ls.y) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(rp.x - 4.f * s, rp.y - 2.f * s));
    reset = ImGui::InvisibleButton("##reset", ImVec2(ls.x + 8.f * s, ls.y + 4.f * s));
    bool hov = ImGui::IsItemHovered();
    dl->AddText(rp, hov ? pal::kText : pal::kFaint, lbl);
    if (hov) dl->AddLine(ImVec2(rp.x, rp.y + ls.y), ImVec2(rp.x + ls.x, rp.y + ls.y), pal::kText, 1.f);
    if (hov) ImGui::SetTooltip("%s", tr(T_ResetSection));
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, tp.y + th + ImGui::GetStyle().ItemSpacing.y));
  }
  ImGui::Dummy(ImVec2(0, 3.f * s));
  return reset;
}

void uiCardEnd() {
  ImGui::Dummy(ImVec2(0, 6.f * uiScale()));
  ImGui::EndChild();
  ImGui::Dummy(ImVec2(0, 4.f * uiScale()));
}

// A scale like the one engraved on a lens barrel: hairline, ticks every tenth, a taller tick at
// the default, a thin needle for the value. The value itself is set in the DIN face.
bool uiSlider(EditResult& r, int key, const char* id, float* v, float vmin, float vmax, float def, const char* fmt,
              ImU32 gradL, ImU32 gradR, bool hueTrack) {
  float s = uiScale();
  ImGui::PushID(id);
  float w = ImGui::GetContentRegionAvail().x;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool modified = *v != def;

  ImGui::PushStyleColor(ImGuiCol_Text, modified ? pal::kText : pal::kMuted);
  ImGui::TextUnformatted(tr(key));
  ImGui::PopStyleColor();
  bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
  char buf[32];
  snprintf(buf, sizeof(buf), fmt, *v);
  pushFont(g_fontMono);
  float vw = ImGui::CalcTextSize(buf).x;
  float lh = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  float th = ImGui::GetTextLineHeight();
  ImGui::SameLine(0, 0);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + w - vw, p0.y + (th - lh) * 0.5f + 1.f * s));
  pushFont(g_fontMono);
  ImGui::PushStyleColor(ImGuiCol_Text, modified ? pal::kAccent : pal::kFaint);
  ImGui::TextUnformatted(buf);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) reset = true;

  ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + th + 1.f * s));
  float knob = 5.f * s;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2.f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, knob * 2.f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
  ImGui::SetNextItemWidth(w);
  bool ch = ImGui::SliderFloat("##v", v, vmin, vmax, "", ImGuiSliderFlags_NoRoundToFormat);
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(3);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  if (ch) {
    float step = std::strstr(fmt, ".2f") ? 0.01f : (std::strstr(fmt, ".1f") ? 0.1f : 1.f);
    *v = std::round(*v / step) * step;
    if (*v == 0.f) *v = 0.f;  // no "-0"
    r.changed = true;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) r.commit = key;
  if (hov && ImGui::IsMouseDoubleClicked(0)) reset = true;

  // Same geometry ImGui uses to map the mouse, so the needle stays under the cursor.
  const float grabPad = 2.f;
  float x0 = a.x + grabPad + knob, x1 = b.x - grabPad - knob;
  float cy = std::floor((a.y + b.y) * 0.5f) + 0.5f;
  auto xAt = [&](float val) { return x0 + (x1 - x0) * std::clamp((val - vmin) / (vmax - vmin), 0.f, 1.f); };
  float xd = xAt(def), xv = xAt(*v);
  ImU32 tick = hov || act ? pal::kMuted : pal::kFaint;
  for (int i = 0; i <= 10; i++) {
    float x = std::floor(x0 + (x1 - x0) * i / 10.f) + 0.5f;
    float hgt = (i % 5 == 0) ? 3.f * s : 1.5f * s;
    dl->AddLine(ImVec2(x, cy + 2.f * s), ImVec2(x, cy + 2.f * s + hgt), tick, 1.f);
  }
  if (hueTrack) {
    for (int i = 0; i < 6; i++) {
      float r0, g0, b0, r1, g1, b1;
      ImGui::ColorConvertHSVtoRGB(i / 6.f, 0.6f, 0.8f, r0, g0, b0);
      ImGui::ColorConvertHSVtoRGB((i + 1) / 6.f, 0.6f, 0.8f, r1, g1, b1);
      ImU32 c0 = ImGui::GetColorU32(ImVec4(r0, g0, b0, 1)), c1 = ImGui::GetColorU32(ImVec4(r1, g1, b1, 1));
      dl->AddRectFilledMultiColor(ImVec2(x0 + (x1 - x0) * i / 6, cy - 1.f * s), ImVec2(x0 + (x1 - x0) * (i + 1) / 6, cy + 1.f * s),
                                  c0, c1, c1, c0);
    }
  } else if (gradL || gradR) {
    dl->AddRectFilledMultiColor(ImVec2(x0, cy - 1.f * s), ImVec2(x1, cy + 1.f * s), gradL, gradR, gradR, gradL);
  } else {
    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), pal::kTrack, 1.f);
    if (std::fabs(xv - xd) > 0.5f)
      dl->AddRectFilled(ImVec2(std::min(xd, xv), cy - 1.f * s), ImVec2(std::max(xd, xv), cy + 1.f * s), pal::kAccent);
  }
  // default mark: a taller tick below the line
  dl->AddLine(ImVec2(std::floor(xd) + 0.5f, cy + 2.f * s), ImVec2(std::floor(xd) + 0.5f, cy + 7.f * s), pal::kMuted, 1.f);
  // index mark: a small wedge resting on the scale, like the one on a focus ring
  float nx = xv, tw = (hov || act) ? 5.f * s : 4.f * s, tht = tw * 1.5f;
  ImU32 nc = act ? pal::kAccent : pal::kText;
  dl->AddTriangleFilled(ImVec2(nx - tw, cy - tht - 1.f * s), ImVec2(nx + tw, cy - tht - 1.f * s), ImVec2(nx, cy), nc);

  if (reset) {
    // Double-click resets; drop the active slider so the mouse can't drag it away again.
    ImGui::ClearActiveID();
    if (*v != def) {
      *v = def;
      r.changed = true;
      r.commit = key;
    }
  }
  ImGui::PopID();
  return ch;
}

void uiHistogram(const unsigned hist[4][256], float scale, float w, float h, bool overlay) {
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (overlay) {
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(20, 19, 17, 255), 2.f * scale);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), pal::kFrame, 2.f * scale);
  }
  if (!overlay) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), pal::kCanvas, 2.f * scale);
  unsigned mx = 1;
  for (int c = 0; c < 4; c++)
    for (int i = 1; i < 255; i++) mx = std::max(mx, hist[c][i]);
  float pad = 6.f * scale, iw = w - pad * 2, ih = h - pad * 2;
  float bw = iw / 256.f;
  const ImU32 cols[3] = {IM_COL32(230, 96, 80, 90), IM_COL32(120, 200, 120, 80), IM_COL32(100, 140, 230, 90)};
  for (int c = 0; c < 3; c++)
    for (int i = 0; i < 256; i++) {
      float v = std::min(1.f, std::sqrt((float)hist[c][i] / mx));
      if (v <= 0.f) continue;
      float x = p0.x + pad + i * bw;
      dl->AddRectFilled(ImVec2(x, p0.y + pad + ih - v * ih), ImVec2(x + bw + 0.5f, p0.y + pad + ih), cols[c]);
    }
  // luminance as a paper-white outline on top
  ImVec2 pts[64];
  for (int k = 0; k < 64; k++) {
    unsigned sum = 0;
    for (int i = k * 4; i < k * 4 + 4; i++) sum += hist[3][i];
    float v = std::min(1.f, std::sqrt((float)sum / 4.f / mx));
    pts[k] = ImVec2(p0.x + pad + (k + 0.5f) * iw / 64.f, p0.y + pad + ih - v * ih);
  }
  dl->AddPolyline(pts, 64, IM_COL32(234, 229, 219, 170), 0, 1.f);
  // quarter marks along the base
  for (int i = 0; i <= 4; i++) {
    float x = std::floor(p0.x + pad + iw * i / 4.f) + 0.5f;
    dl->AddLine(ImVec2(x, p0.y + pad + ih), ImVec2(x, p0.y + pad + ih + 3.f * scale), pal::kFaint, 1.f);
  }
  unsigned total = 0;
  for (int i = 0; i < 256; i++) total += hist[3][i];
  bool clipLo = total && (hist[0][0] + hist[1][0] + hist[2][0]) > total * 3 / 1000;
  bool clipHi = total && (hist[0][255] + hist[1][255] + hist[2][255]) > total * 3 / 1000;
  float d = 2.5f * scale;
  dl->AddRectFilled(ImVec2(p0.x + pad, p0.y + pad), ImVec2(p0.x + pad + d * 2, p0.y + pad + d * 2),
                    clipLo ? IM_COL32(110, 160, 255, 255) : pal::kFaint);
  dl->AddRectFilled(ImVec2(p0.x + w - pad - d * 2, p0.y + pad), ImVec2(p0.x + w - pad, p0.y + pad + d * 2),
                    clipHi ? pal::kAccent : pal::kFaint);
}

// Resets a group of float fields to their defaults as one history entry.
static void resetFields(EditParams& p, EditResult& r, int key, std::initializer_list<float EditParams::*> fields) {
  const EditParams d;
  bool any = false;
  for (auto f : fields)
    if (p.*f != d.*f) p.*f = d.*f, any = true;
  if (any) r.changed = true, r.commit = key;
}

void uiLight(EditParams& p, EditResult& r, bool reset) {
  if (reset)
    resetFields(p, r, T_CardLight, {&EditParams::exposure, &EditParams::contrast, &EditParams::highlights,
                                    &EditParams::shadows, &EditParams::whites, &EditParams::blacks});
  uiSlider(r, T_Exposure, "exp", &p.exposure, -5, 5, 0, "%+.2f");
  uiSlider(r, T_Contrast, "con", &p.contrast, -100, 100, 0, "%.0f");
  uiSlider(r, T_Highlights, "hil", &p.highlights, -100, 100, 0, "%.0f");
  uiSlider(r, T_Shadows, "sha", &p.shadows, -100, 100, 0, "%.0f");
  uiSlider(r, T_Whites, "whi", &p.whites, -100, 100, 0, "%.0f");
  uiSlider(r, T_Blacks, "bla", &p.blacks, -100, 100, 0, "%.0f");
}

void uiWhiteBalance(EditParams& p, EditResult& r, bool reset) {
  if (reset) resetFields(p, r, T_WhiteBalance, {&EditParams::temp, &EditParams::tint});
  uiSlider(r, T_Temp, "temp", &p.temp, -100, 100, 0, "%.0f", IM_COL32(70, 120, 235, 255), IM_COL32(235, 200, 60, 255));
  uiSlider(r, T_Tint, "tint", &p.tint, -100, 100, 0, "%.0f", IM_COL32(70, 200, 80, 255), IM_COL32(215, 70, 200, 255));
}

void uiVividness(EditParams& p, EditResult& r, bool reset) {
  if (reset) resetFields(p, r, T_CardVivid, {&EditParams::vibrance, &EditParams::saturation});
  uiSlider(r, T_Vibrance, "vib", &p.vibrance, -100, 100, 0, "%.0f");
  uiSlider(r, T_Saturation, "sat", &p.saturation, -100, 100, 0, "%.0f", IM_COL32(128, 128, 128, 255),
           IM_COL32(230, 60, 60, 255));
}

void uiDefinition(EditParams& p, EditResult& r, bool reset) {
  if (reset) resetFields(p, r, T_Presence, {&EditParams::texture, &EditParams::clarity, &EditParams::dehaze});
  uiSlider(r, T_Texture, "tex", &p.texture, -100, 100, 0, "%.0f");
  uiSlider(r, T_Clarity, "cla", &p.clarity, -100, 100, 0, "%.0f");
  uiSlider(r, T_Dehaze, "deh", &p.dehaze, -100, 100, 0, "%.0f");
}

void uiSharpen(EditParams& p, EditResult& r, bool reset) {
  if (reset)
    resetFields(p, r, T_Sharpening, {&EditParams::sharpAmount, &EditParams::sharpRadius, &EditParams::sharpMasking});
  uiSlider(r, T_Amount, "sa", &p.sharpAmount, 0, 150, 0, "%.0f");
  uiSlider(r, T_Radius, "sr", &p.sharpRadius, 0.5f, 3.f, 1.f, "%.1f");
  uiSlider(r, T_Masking, "sm", &p.sharpMasking, 0, 100, 0, "%.0f");
}

void uiNoise(EditParams& p, EditResult& r, bool reset) {
  if (reset) resetFields(p, r, T_NoiseReduction, {&EditParams::nrLum, &EditParams::nrColor});
  uiSlider(r, T_Luminance, "nl", &p.nrLum, 0, 100, 0, "%.0f");
  uiSlider(r, T_Color, "nc", &p.nrColor, 0, 100, 0, "%.0f");
}

void uiVignette(EditParams& p, EditResult& r, bool reset) {
  if (reset)
    resetFields(p, r, T_Vignette,
                {&EditParams::vigAmount, &EditParams::vigMidpoint, &EditParams::vigFeather, &EditParams::vigRoundness});
  uiSlider(r, T_Amount, "va", &p.vigAmount, -100, 100, 0, "%.0f", IM_COL32(20, 20, 20, 255), IM_COL32(235, 235, 235, 255));
  uiSlider(r, T_Midpoint, "vm", &p.vigMidpoint, 0, 100, 50, "%.0f");
  uiSlider(r, T_Feather, "vf", &p.vigFeather, 0, 100, 50, "%.0f");
  uiSlider(r, T_Roundness, "vr", &p.vigRoundness, -100, 100, 0, "%.0f");
}

void uiGrain(EditParams& p, EditResult& r, bool reset) {
  if (reset)
    resetFields(p, r, T_Grain, {&EditParams::grainAmount, &EditParams::grainSize, &EditParams::grainRoughness});
  uiSlider(r, T_Amount, "ga", &p.grainAmount, 0, 100, 0, "%.0f");
  uiSlider(r, T_Size, "gs", &p.grainSize, 0, 100, 25, "%.0f");
  uiSlider(r, T_Roughness, "gr", &p.grainRoughness, 0, 100, 50, "%.0f");
}

void uiToneCurve(EditParams& p, EditResult& r, PanelState& s, const unsigned hist[4][256], float scale, bool reset) {
  const char* names[4] = {"RGB", "R", "G", "B"};
  const ImU32 cols[4] = {IM_COL32(230, 230, 230, 255), IM_COL32(240, 90, 90, 255), IM_COL32(90, 220, 90, 255),
                         IM_COL32(100, 140, 255, 255)};
  if (reset) {
    EditParams d;
    if (!sameCurves(p, d)) {
      std::memcpy(p.curve, d.curve, sizeof(p.curve));
      r.changed = true;
      r.commit = T_ToneCurve;
    }
  }
  int k = uiSegmented("curvech", names, 4, s.curveChannel);
  if (k >= 0) s.curveChannel = k;
  Curve& c = p.curve[s.curveChannel];
  ImGui::Dummy(ImVec2(0, 2.f * scale));

  float size = ImGui::GetContentRegionAvail().x;
  ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size, p0.y + size);
  ImGui::InvisibleButton("##curve", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, pal::kCanvas, 2.f * scale);
  dl->AddRect(p0, p1, pal::kFrame, 2.f * scale);
  const unsigned* hh = hist[s.curveChannel == 0 ? 3 : s.curveChannel - 1];
  unsigned mx = 1;
  for (int i = 1; i < 255; i++) mx = std::max(mx, hh[i]);
  for (int i = 0; i < 256; i++) {
    float v = std::min(1.f, (float)hh[i] / mx) * 0.9f;
    float x = p0.x + (i + 0.5f) * size / 256.f;
    dl->AddLine(ImVec2(x, p1.y), ImVec2(x, p1.y - v * size), IM_COL32(255, 255, 255, 20), size / 256.f + 0.5f);
  }
  for (int i = 1; i < 4; i++) {
    float t = i / 4.f;
    dl->AddLine(ImVec2(p0.x + t * size, p0.y), ImVec2(p0.x + t * size, p1.y), IM_COL32(255, 255, 255, 25));
    dl->AddLine(ImVec2(p0.x, p0.y + t * size), ImVec2(p1.x, p0.y + t * size), IM_COL32(255, 255, 255, 25));
  }
  dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p0.y), IM_COL32(255, 255, 255, 35));
  ImVec2 pts[97];
  for (int i = 0; i <= 96; i++) {
    float x = i / 96.f;
    pts[i] = ImVec2(p0.x + x * size, p1.y - evalCurve(c, x) * size);
  }
  dl->AddPolyline(pts, 97, cols[s.curveChannel], 0, 2.f * scale);

  ImVec2 m = ImGui::GetIO().MousePos;
  float mxn = std::clamp((m.x - p0.x) / size, 0.f, 1.f), myn = std::clamp(1.f - (m.y - p0.y) / size, 0.f, 1.f);
  int nearest = -1;
  float best = (9.f * scale) * (9.f * scale);
  for (int i = 0; i < c.n; i++) {
    float dx = p0.x + c.p[i].x * size - m.x, dy = p1.y - c.p[i].y * size - m.y;
    if (dx * dx + dy * dy < best) {
      best = dx * dx + dy * dy;
      nearest = i;
    }
  }
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) {
    if (nearest >= 0) {
      s.curveDrag = nearest;
    } else {
      s.curveDrag = curveInsert(c, mxn, evalCurve(c, mxn));
      if (s.curveDrag >= 0) r.changed = true;
    }
  }
  if (active && s.curveDrag >= 0 && s.curveDrag < c.n && ImGui::IsMouseDown(0)) {
    int i = s.curveDrag;
    float lo = i == 0 ? 0.f : c.p[i - 1].x + 0.01f, hi = i == c.n - 1 ? 1.f : c.p[i + 1].x - 0.01f;
    float nx = std::clamp(mxn, lo, std::max(lo, hi)), ny = myn;
    if (nx != c.p[i].x || ny != c.p[i].y) {
      c.p[i] = {nx, ny};
      r.changed = true;
    }
  }
  if (ImGui::IsItemDeactivated() && s.curveDrag >= 0) {
    r.commit = T_ToneCurve;
    s.curveDrag = -1;
  }
  if (hovered && ImGui::IsMouseClicked(1) && nearest >= 0 && c.n > 2) {
    curveRemove(c, nearest);
    r.changed = true;
    r.commit = T_ToneCurve;
  }
  for (int i = 0; i < c.n; i++) {
    ImVec2 q(p0.x + c.p[i].x * size, p1.y - c.p[i].y * size);
    bool hot = i == nearest || i == s.curveDrag;
    dl->AddCircleFilled(q, (hot ? 5.f : 4.f) * scale, hot ? IM_COL32(255, 255, 255, 255) : cols[s.curveChannel]);
    dl->AddCircle(q, (hot ? 5.f : 4.f) * scale, IM_COL32(20, 20, 20, 255));
  }
  if (s.curveDrag >= 0 && s.curveDrag < c.n)
    ImGui::SetTooltip("%.0f / %.0f", c.p[s.curveDrag].x * 255.f, c.p[s.curveDrag].y * 255.f);
  uiHint(tr(T_CurveHelp));
}

static const int kBandKeys[kHslBands] = {T_Red, T_Orange, T_Yellow, T_Green, T_Aqua, T_Blue, T_Purple, T_Magenta};
static const float kBandHue[kHslBands] = {0, 30, 60, 120, 180, 240, 270, 300};

static ImU32 hueCol(float deg, float s = 0.75f, float v = 0.85f) {
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(std::fmod(deg + 360.f, 360.f) / 360.f, s, v, r, g, b);
  return ImGui::GetColorU32(ImVec4(r, g, b, 1.f));
}

void uiColorMixer(EditParams& p, EditResult& r, PanelState& s, bool reset) {
  if (reset) {
    bool any = false;
    for (int b = 0; b < kHslBands; b++) any |= p.hslHue[b] != 0 || p.hslSat[b] != 0 || p.hslLum[b] != 0;
    if (any) {
      for (int b = 0; b < kHslBands; b++) p.hslHue[b] = p.hslSat[b] = p.hslLum[b] = 0;
      r.changed = true;
      r.commit = T_ColorMixer;
    }
  }
  const char* modes[3] = {tr(T_Hue), tr(T_Saturation), tr(T_Luminance)};
  int k = uiSegmented("hslmode", modes, 3, s.hslMode);
  if (k >= 0) s.hslMode = k;
  for (int b = 0; b < kHslBands; b++) {
    char id[16];
    snprintf(id, sizeof(id), "hsl%d", b);
    float h = kBandHue[b];
    if (s.hslMode == 0)
      uiSlider(r, kBandKeys[b], id, &p.hslHue[b], -100, 100, 0, "%.0f", hueCol(h - 30), hueCol(h + 30));
    else if (s.hslMode == 1)
      uiSlider(r, kBandKeys[b], id, &p.hslSat[b], -100, 100, 0, "%.0f", hueCol(h, 0.05f, 0.6f), hueCol(h, 0.95f, 0.9f));
    else
      uiSlider(r, kBandKeys[b], id, &p.hslLum[b], -100, 100, 0, "%.0f", hueCol(h, 0.8f, 0.25f), hueCol(h, 0.45f, 1.f));
  }
}

static void colorWheel(EditResult& r, GradeWheel& g, float rad, int key) {
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##wheel", ImVec2(rad * 2, rad * 2));
  ImVec2 c(p0.x + rad, p0.y + rad);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int N = 72;
  ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve(N * 3, N * 3);
  for (int i = 0; i < N; i++) {
    float a0 = i * 6.2831853f / N, a1 = (i + 1) * 6.2831853f / N;
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx((ImDrawIdx)(base + 1));
    dl->PrimWriteIdx((ImDrawIdx)(base + 2));
    dl->PrimWriteVtx(c, uv, IM_COL32(120, 120, 120, 255));
    dl->PrimWriteVtx(ImVec2(c.x + std::cos(a0) * rad, c.y - std::sin(a0) * rad), uv, hueCol(i * 360.f / N));
    dl->PrimWriteVtx(ImVec2(c.x + std::cos(a1) * rad, c.y - std::sin(a1) * rad), uv, hueCol((i + 1) * 360.f / N));
  }
  dl->AddCircle(c, rad, pal::kCard, 64, 2.f);
  float a = g.hue * 3.14159265f / 180.f, d = g.sat / 100.f * rad;
  ImVec2 mk(c.x + std::cos(a) * d, c.y - std::sin(a) * d);
  dl->AddCircleFilled(mk, 5.f, hueCol(g.hue, g.sat / 100.f, 0.9f));
  dl->AddCircle(mk, 6.f, IM_COL32(255, 255, 255, 255), 0, 2.f);

  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
    ImGui::ClearActiveID();
    g.hue = g.sat = 0;
    r.changed = true;
    r.commit = key;
    return;
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
    ImVec2 m = ImGui::GetIO().MousePos;
    float vx = m.x - c.x, vy = m.y - c.y;
    float dist = std::min(1.f, std::sqrt(vx * vx + vy * vy) / rad);
    float hue = std::atan2(-vy, vx) * 180.f / 3.14159265f;
    if (hue < 0) hue += 360.f;
    g.hue = std::round(hue);
    g.sat = std::round(dist * 100.f);
    r.changed = true;
  }
  if (ImGui::IsItemDeactivated()) r.commit = key;
}

void uiColorGrading(EditParams& p, EditResult& r, PanelState& s, float scale, bool reset) {
  const int keys[GRADE_COUNT] = {T_Shadows, T_Midtones, T_Highlights, T_Global};
  if (reset) {
    EditParams d;
    if (std::memcmp(p.grade, d.grade, sizeof(p.grade)) != 0 || p.gradeBlending != d.gradeBlending ||
        p.gradeBalance != d.gradeBalance) {
      std::memcpy(p.grade, d.grade, sizeof(p.grade));
      p.gradeBlending = d.gradeBlending;
      p.gradeBalance = d.gradeBalance;
      r.changed = true;
      r.commit = T_ColorGrading;
    }
  }
  const char* labels[GRADE_COUNT];
  for (int i = 0; i < GRADE_COUNT; i++) labels[i] = tr(keys[i]);
  int k = uiSegmented("gradesel", labels, GRADE_COUNT, s.gradeSel);
  if (k >= 0) s.gradeSel = k;
  ImGui::Dummy(ImVec2(0, 4.f * scale));
  GradeWheel& g = p.grade[s.gradeSel];
  float avail = ImGui::GetContentRegionAvail().x;
  float rad = std::min(avail * 0.5f - 4.f, 80.f * scale);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - rad * 2) * 0.5f);
  ImGui::PushID(s.gradeSel);
  colorWheel(r, g, rad, keys[s.gradeSel]);
  uiSlider(r, T_Hue, "gh", &g.hue, 0, 360, 0, "%.0f", 0, 0, true);
  uiSlider(r, T_Saturation, "gs", &g.sat, 0, 100, 0, "%.0f");
  uiSlider(r, T_Luminance, "gl", &g.lum, -100, 100, 0, "%.0f", IM_COL32(30, 30, 30, 255), IM_COL32(230, 230, 230, 255));
  ImGui::PopID();
  ImGui::Dummy(ImVec2(0, 4.f * scale));
  uiSlider(r, T_Blending, "gbl", &p.gradeBlending, 0, 100, 50, "%.0f");
  uiSlider(r, T_Balance, "gba", &p.gradeBalance, -100, 100, 0, "%.0f");
}

static ImVec2 g_tourA[TR_COUNT], g_tourB[TR_COUNT];
static bool g_tourHas[TR_COUNT];
void tourClear() { std::memset(g_tourHas, 0, sizeof(g_tourHas)); }
void tourSet(int id, ImVec2 a, ImVec2 b) {
  if (id < 0 || id >= TR_COUNT) return;
  g_tourA[id] = a, g_tourB[id] = b, g_tourHas[id] = true;
}
void tourSetItem(int id) { tourSet(id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }
void tourSetWindow(int id) {
  ImVec2 p = ImGui::GetWindowPos(), z = ImGui::GetWindowSize();
  tourSet(id, p, ImVec2(p.x + z.x, p.y + z.y));
}
bool tourGet(int id, ImVec2& a, ImVec2& b) {
  if (id < 0 || id >= TR_COUNT || !g_tourHas[id]) return false;
  a = g_tourA[id], b = g_tourB[id];
  return true;
}
