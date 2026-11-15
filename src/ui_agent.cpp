#include "ui_agent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "i18n.h"
#include "imgui.h"
#include "platform_win.h"
#include "ui_panels.h"

namespace {

struct Name {
  const char* key;
  const char* en;
  const char* th;
};

const Name kTools[] = {
    {"open_photo", "Open photo", "เปิดภาพ"},
    {"view", "Look at the photo", "ดูภาพ"},
    {"get_info", "Read photo info", "อ่านข้อมูลภาพ"},
    {"list_adjustments", "List adjustments", "ดูรายการปรับแต่ง"},
    {"set_adjustments", "Adjust light & colour", "ปรับแสงและสี"},
    {"auto_edit", "Auto edit (AI)", "แต่งอัตโนมัติ (AI)"},
    {"apply_look", "Apply a look", "ใส่สไตล์ภาพ"},
    {"crop_rotate", "Crop & rotate", "ครอปและหมุน"},
    {"find_regions", "Find regions (AI)", "หาส่วนต่างๆ ในภาพ (AI)"},
    {"adjust_region", "Adjust one area", "ปรับเฉพาะจุด"},
    {"recover_highlights", "Recover highlights (AI)", "กู้ส่วนที่สว่างเกิน (AI)"},
    {"magic_erase", "Magic eraser", "ยางลบมหัศจรรย์"},
    {"layer_add", "Add photo layer", "เพิ่มเลเยอร์ภาพ"},
    {"layer_update", "Edit photo layer", "แก้เลเยอร์ภาพ"},
    {"layer_remove", "Remove photo layer", "ลบเลเยอร์ภาพ"},
    {"undo", "Undo", "ย้อนกลับ"},
    {"export", "Export", "ส่งออกภาพ"},
    {"cull_folder", "Cull a folder (AI)", "คัดภาพทั้งโฟลเดอร์ (AI)"},
    {"search_photos", "Search photos", "ค้นหาภาพ"},
    {"batch_auto_edit", "Auto edit many photos", "แต่งหลายภาพอัตโนมัติ"},
};

// Settings without a slider label of their own in i18n.h.
const Name kKeys[] = {
    {"hsl", "Colour mix (HSL)", "ผสมสี (HSL)"},
    {"curve", "Tone curve", "เส้นโค้ง"},
    {"grade", "Colour grading", "เกรดสี"},
    {"region", "Local adjustment", "ปรับเฉพาะจุด"},
    {"layer", "Photo layers", "เลเยอร์ภาพ"},
    {"heals", "Magic eraser", "ยางลบมหัศจรรย์"},
    {"recover", "Highlight recovery", "กู้ส่วนที่สว่างเกิน"},
    {"sharpRadius", "Sharpen radius", "รัศมีความคม"},
    {"sharpMasking", "Sharpen edges only", "คมเฉพาะขอบ"},
    {"nrLum", "Noise reduction", "ลดนอยส์"},
    {"nrColor", "Colour noise reduction", "ลดนอยส์สี"},
    {"vigMidpoint", "Vignette size", "ขนาดขอบมืด"},
    {"vigFeather", "Vignette softness", "ความนุ่มขอบมืด"},
    {"vigRoundness", "Vignette shape", "รูปทรงขอบมืด"},
    {"grainSize", "Grain size", "ขนาดเกรน"},
    {"grainRoughness", "Grain roughness", "ความหยาบเกรน"},
};

struct Slider {
  const char* key;
  int label;
};
const Slider kSliders[] = {{"exposure", T_Exposure},   {"contrast", T_Contrast},     {"highlights", T_Highlights},
                           {"shadows", T_Shadows},     {"whites", T_Whites},         {"blacks", T_Blacks},
                           {"temp", T_Temp},           {"tint", T_Tint},             {"vibrance", T_Vibrance},
                           {"saturation", T_Saturation}, {"texture", T_Texture},     {"clarity", T_Clarity},
                           {"dehaze", T_Dehaze},       {"sharpAmount", T_Sharpening}, {"vigAmount", T_Vignette},
                           {"grainAmount", T_Grain},   {"crop", T_Crop}};

bool thai() { return currentLang() == Lang::TH; }

void spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  float t = (float)ImGui::GetTime() * 6.f;
  dl->PathArcTo(c, r, t, t + 4.2f, 16);
  dl->PathStroke(col, 0, std::max(1.5f, r * 0.3f));
}

void tick(ImDrawList* dl, ImVec2 c, float r, bool ok) {
  if (ok) {
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.7f), pal::kMuted, 1.6f);
    dl->AddLine(ImVec2(c.x - r * 0.3f, c.y + r * 0.7f), ImVec2(c.x + r, c.y - r * 0.8f), pal::kMuted, 1.6f);
  } else {
    dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), pal::kAccent, 1.6f);
    dl->AddLine(ImVec2(c.x + r * 0.8f, c.y - r * 0.8f), ImVec2(c.x - r * 0.8f, c.y + r * 0.8f), pal::kAccent, 1.6f);
  }
}

void muted(const std::string& s, ImU32 col = pal::kMuted) {
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
}

std::string samePath(const std::string& p) {
  std::string o = p;
  for (char& c : o) c = c == '/' ? '\\' : (char)tolower((unsigned char)c);
  return o;
}

}  // namespace

const char* agentToolName(const std::string& tool) {
  for (const Name& n : kTools)
    if (tool == n.key) return thai() ? n.th : n.en;
  return tool.c_str();
}

std::string agentChangeText(const AgentChange& c) {
  std::string key = c.key, label;
  if (key.rfind("region", 0) == 0) key = "region";
  for (const Slider& s : kSliders)
    if (key == s.key) label = tr(s.label);
  for (const Name& n : kKeys)
    if (key == n.key) label = thai() ? n.th : n.en;
  if (label.empty()) label = c.key;
  if (key == "region") {  // "face, sky" -> the region names the editor uses
    static const char* en[] = {"person", "sky",     "road",   "building", "trees", "grass", "water",
                               "mountain", "ground", "vehicle", "animal", "walls", "food", "face"};
    static const int keys[] = {T_Reg_Person, T_Reg_Sky,     T_Reg_Road,    T_Reg_Building, T_Reg_Trees,
                               T_Reg_Grass,  T_Reg_Water,   T_Reg_Mountain, T_Reg_Ground,  T_Reg_Vehicle,
                               T_Reg_Animal, T_Reg_Walls,   T_Reg_Food,    T_Reg_Face};
    std::string out, part;
    std::string list = c.after + ",";
    for (char ch : list) {
      if (ch != ',') {
        if (ch != ' ') part += ch;
        continue;
      }
      if (part.empty()) continue;
      std::string name = part;
      for (int i = 0; i < 14; i++)
        if (part == en[i]) name = tr(keys[i]);
      out += (out.empty() ? "" : ", ") + name;
      part.clear();
    }
    return label + ": " + (out.empty() ? "-" : out);
  }
  if (key == "layer") return label + ": " + (c.after.empty() ? "-" : c.after);
  if (c.before.empty() && c.after.empty()) return label + " (" + tr(T_AgentChanged) + ")";
  auto num = [](const std::string& v) { return v.empty() ? std::string("0") : v.size() > 12 ? v.substr(0, 12) : v; };
  return label + "  " + num(c.before) + " \xe2\x86\x92 " + num(c.after);
}

bool AgentPanel::animating() const {
  for (const AgentSession& s : watch.sessions())
    if (!s.alive || s.current()) return true;
  return false;
}

std::string AgentPanel::clientName() const {
  for (const AgentSession& s : watch.sessions())
    if (s.alive) return s.client;
  return std::string();
}

void AgentPanel::draw(AgentHost& h) {
  const std::vector<AgentSession>& ss = watch.sessions();
  if (ss.size() > seenSessions_) hidden_ = false;  // a new agent connected: show it
  seenSessions_ = ss.size();
  if (ss.empty() || hidden_) return;
  float s = h.scale;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20 * s, vp->WorkPos.y + vp->WorkSize.y - 96 * s - h.bottomGap),
      ImGuiCond_Always, ImVec2(1, 1));
  ImGui::SetNextWindowSize(ImVec2(390 * s, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 27, 25, 245));
  ImGui::PushStyleColor(ImGuiCol_Border, pal::kFrame);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 12 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 4 * s));
  ImGui::Begin("##agentcard", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float lh = ImGui::GetTextLineHeight();
  int perSession = ss.size() == 1 ? 4 : 2;
  for (size_t si = 0; si < ss.size(); si++) {
    const AgentSession& S = ss[si];
    ImGui::PushID((int)S.pid);
    if (si > 0) ImGui::Separator();
    const AgentEvent* cur = S.current();
    // ---- header: live dot, AI app, state; collapse / hide on the first one
    ImVec2 p = ImGui::GetCursorScreenPos();
    float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.f);
    ImU32 dot = !S.alive ? pal::kFaint : cur ? pal::kAccent : IM_COL32(110, 190, 120, 255);
    if (S.alive && cur) dl->AddCircleFilled(ImVec2(p.x + 5 * s, p.y + lh * 0.5f), 5 * s + 4 * s * pulse, IM_COL32(226, 82, 54, 50));
    dl->AddCircleFilled(ImVec2(p.x + 5 * s, p.y + lh * 0.5f), 5 * s, dot);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 18 * s, p.y));
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted(S.client.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, S.alive ? pal::kMuted : pal::kFaint);
    ImGui::TextUnformatted(tr(S.alive ? T_AgentConnected : T_AgentGone));
    ImGui::PopStyleColor();
    if (si == 0) {
      float bw = ImGui::GetFrameHeight();
      ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bw * 2 - 4 * s);
      if (ImGui::SmallButton(collapsed_ ? "+##agcol" : "-##agcol")) collapsed_ = !collapsed_;
      ImGui::SameLine(0, 4 * s);
      if (ImGui::SmallButton("x##aghide")) hidden_ = true;
    }
    // ---- what it is doing right now
    if (S.alive) {
      if (cur) {
        ImVec2 q = ImGui::GetCursorScreenPos();
        spinner(dl, ImVec2(q.x + 6 * s, q.y + lh * 0.5f), 5 * s, pal::kAccent);
        ImGui::SetCursorScreenPos(ImVec2(q.x + 18 * s, q.y));
        std::string line = std::string(tr(T_AgentNow)) + ": " + agentToolName(cur->tool);
        ImGui::PushStyleColor(ImGuiCol_Text, pal::kText);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        std::string sub = cur->photo.empty() ? std::string() : fileName(cur->photo);
        if (!cur->detail.empty()) sub += (sub.empty() ? "" : "  \xc2\xb7  ") + cur->detail;
        if (!sub.empty()) {
          ImGui::PushFont(g_fontSmall);
          ImGui::Indent(18 * s);
          muted(sub);
          ImGui::Unindent(18 * s);
          ImGui::PopFont();
        }
      } else {
        ImGui::Indent(18 * s);
        muted(tr(T_AgentIdle));
        ImGui::Unindent(18 * s);
      }
    }
    // ---- finished steps, newest first, with what each changed
    if (!collapsed_) {
      int shown = 0;
      bool header = false;
      for (auto it = S.events.rbegin(); it != S.events.rend() && shown < perSession; ++it) {
        const AgentEvent& e = *it;
        if (e.running) continue;
        if (!header) {
          ImGui::Dummy(ImVec2(0, 4 * s));
          ImGui::PushFont(g_fontSmall);
          muted(tr(T_AgentRecent), pal::kFaint);
          ImGui::PopFont();
          header = true;
        }
        shown++;
        ImGui::PushID(shown);
        ImVec2 q = ImGui::GetCursorScreenPos();
        tick(dl, ImVec2(q.x + 6 * s, q.y + lh * 0.5f), 4.5f * s, e.ok);
        ImGui::SetCursorScreenPos(ImVec2(q.x + 18 * s, q.y));
        std::string title = agentToolName(e.tool);
        if (!e.photo.empty()) title += "  \xc2\xb7  " + fileName(e.photo);
        ImGui::TextUnformatted(title.c_str());
        if (!e.photo.empty() && h.openPhoto && samePath(e.photo) != samePath(h.openPath) &&
            fileExists(e.photo) && !isDirectory(e.photo)) {
          ImGui::SameLine();
          if (ImGui::SmallButton(trId(T_AgentOpenPhoto, "agopen"))) h.openPhoto(e.photo);
        }
        ImGui::PushFont(g_fontSmall);
        ImGui::Indent(18 * s);
        if (!e.ok) {
          muted(std::string(tr(T_AgentFailed)) + (e.msg.empty() ? "" : ": " + e.msg), pal::kAccent);
        } else if (!e.changes.empty()) {
          size_t n = std::min<size_t>(e.changes.size(), 4);
          for (size_t i = 0; i < n; i++) muted(agentChangeText(e.changes[i]), pal::kText);
          if (e.changes.size() > n) {
            char b[64];
            snprintf(b, sizeof(b), tr(T_AgentMore), (int)(e.changes.size() - n));
            muted(b);
          }
        } else if (!e.msg.empty() && (e.tool == "export" || e.tool == "cull_folder" || e.tool == "search_photos" || e.tool == "batch_auto_edit" || e.tool == "find_regions")) {
          muted(e.msg);
        }
        ImGui::Unindent(18 * s);
        ImGui::PopFont();
        ImGui::PopID();
      }
    }
    ImGui::PopID();
  }
  if (!collapsed_ && connected()) {
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2 * s, 1 * s));
    ImGui::Checkbox(trId(T_AgentFollow, "agfollow"), &follow);
    ImGui::PopStyleVar();
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}
