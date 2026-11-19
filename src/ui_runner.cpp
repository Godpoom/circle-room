#include "ui_runner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "i18n.h"
#include "imgui.h"
#include "platform_win.h"
#include "ui_icons.h"
#include "ui_panels.h"

namespace {

// The play field, in logical pixels (scaled with the UI).
const float kW = 720.f, kH = 250.f, kGround = 40.f;  // ground line kGround above the bottom
const float kPx = 96.f, kR = 19.f;                    // the runner's x and radius
const float kGravity = 2900.f, kJump = 880.f, kCut = 2.6f;  // kCut: extra fall when the key is let go early

std::string bestFile() {
  const char* la = getenv("LOCALAPPDATA");
  if (!la) return "";
  std::string d = std::string(la) + "\\CircleRoom";
  ensureDir(d);
  return d + "\\run_best.txt";
}

int loadBest() {
  std::string t, f = bestFile();
  if (f.empty() || !readTextFile(f, t)) return 0;
  return std::max(0, atoi(t.c_str()));
}

void saveBest(int b) {
  std::string f = bestFile();
  if (!f.empty()) writeTextFile(f, std::to_string(b));
}

float frand(Runner& r, float a, float b) {
  r.rng = r.rng * 1664525u + 1013904223u;
  return a + (b - a) * ((r.rng >> 8) / 16777216.f);
}

void reset(Runner& r) {
  r.state = 0;
  r.y = r.vy = 0;
  r.speed = 390.f;
  r.dist = 0;
  r.t = 0;
  r.score = 0;
  r.nextGap = 520.f;
  r.obs.clear();
}

void spawn(Runner& r) {
  RunObstacle o{};
  float roll = frand(r, 0, 1);
  if (r.score > 250 && roll < 0.22f) {  // a lamp hanging at head height: stay down, or clear it high
    o.kind = 2;
    o.w = 34;
    o.h = 26;
    o.lift = frand(r, 52, 64);
  } else if (roll < 0.62f) {
    o.kind = 0;  // tripod
    o.w = 28;
    o.h = frand(r, 42, 60);
  } else {
    o.kind = 1;  // film canisters
    o.w = frand(r, 32, 60);
    o.h = frand(r, 24, 32);
  }
  o.x = kW + 20.f;
  r.obs.push_back(o);
  float fast = std::min(1.f, (r.speed - 390.f) / 560.f);
  r.nextGap = r.speed * frand(r, 0.62f, 1.25f - 0.3f * fast) + 150.f;
}

bool hits(const Runner& r, const RunObstacle& o) {
  float cx = kPx, cy = r.y + kR;  // centre, height above the ground
  float x0 = o.x + 3, x1 = o.x + o.w - 3, y0 = o.lift + 2, y1 = o.lift + o.h - 2;
  float nx = std::clamp(cx, x0, x1), ny = std::clamp(cy, y0, y1);
  float dx = cx - nx, dy = cy - ny;
  return dx * dx + dy * dy < (kR - 3) * (kR - 3);
}

void step(Runner& r, float dt, bool jumpPressed) {
  if (r.state == 1) {
    r.t += dt;
    r.speed = std::min(950.f, r.speed + 11.f * dt);
    float move = r.speed * dt;
    r.dist += move;
    r.score = (int)(r.dist / 12.f);
    if (jumpPressed && r.y <= 0.f) r.vy = kJump;
    r.vy -= kGravity * dt * (r.vy > 0 && !r.holding ? kCut : 1.f);
    r.y += r.vy * dt;
    if (r.y < 0) r.y = r.vy = 0;
    for (RunObstacle& o : r.obs) o.x -= move;
    while (!r.obs.empty() && r.obs.front().x + r.obs.front().w < -40.f) r.obs.erase(r.obs.begin());
    r.nextGap -= move;
    if (r.nextGap <= 0) spawn(r);
    for (const RunObstacle& o : r.obs)
      if (hits(r, o)) {
        r.state = 2;
        r.crashT = r.t;
        if (r.score > r.best) {
          r.best = r.score;
          saveBest(r.best);
        }
        break;
      }
  } else if (jumpPressed && (r.state == 0 || r.t - r.crashT > 0.35f)) {
    bool wasCrash = r.state == 2;
    reset(r);
    r.state = 1;
    if (!wasCrash) r.vy = kJump;  // the first press also jumps
  }
  if (r.state == 2) r.t += dt;
}

}  // namespace

// Self-test: a player who never jumps crashes soon; one who jumps over ground obstacles (and stays
// down under lamps) survives a long run, so the gaps the game makes are always playable.
bool runnerSimCheck(int& idleScore, int& botScore) {
  Runner r;
  r.best = 1 << 30;  // no file writes from the test
  reset(r);
  step(r, 0, true);
  for (int i = 0; i < 60 * 30 && r.state == 1; i++) step(r, 1.f / 60, false);
  idleScore = r.score;
  bool idleCrashed = r.state == 2;
  reset(r);
  step(r, 0, true);
  for (int i = 0; i < 60 * 120 && r.state == 1; i++) {
    bool jump = false;
    for (const RunObstacle& o : r.obs) {
      if (o.x + o.w < kPx - kR) continue;
      float lead = o.x - kPx;  // the next obstacle ahead
      if (o.kind != 2 && lead < r.speed * 0.17f + 10.f) jump = true;
      break;
    }
    r.holding = jump || r.vy > 0;
    step(r, 1.f / 60, jump);
  }
  botScore = r.score;
  return idleCrashed && r.state == 1;
}

void runnerOpen(Runner& r) {
  r.request = true;
  r.open = true;
  if (r.best < 0) r.best = loadBest();
  reset(r);
}

bool runnerLogoClick(double now) {
  static double last = -10;
  static int n = 0;
  n = now - last < 1.2 ? n + 1 : 1;
  last = now;
  if (n < 11) return false;
  n = 0;
  return true;
}

bool drawRunner(Runner& r, float s) {
  if (!r.open) return false;
  if (r.request) {
    ImGui::OpenPopup("###runner");
    r.request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(kW * s + 32.f * s, 0), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool keep = true;
  std::string title = std::string(tr(T_RunTitle)) + "###runner";
  if (!ImGui::BeginPopupModal(title.c_str(), &keep, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
    r.open = false;
    return false;
  }
  ImGuiIO& io = ImGui::GetIO();
  float dt = std::min(io.DeltaTime, 1.f / 30.f);

  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##field", ImVec2(kW * s, kH * s));
  bool click = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  bool down = ImGui::IsItemActive();
  bool key = ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
             ImGui::IsKeyPressed(ImGuiKey_W, false);
  r.holding = down || ImGui::IsKeyDown(ImGuiKey_Space) || ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_W);
  step(r, dt, key || click);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p0.x + kW * s, p0.y + kH * s);
  dl->PushClipRect(p0, p1, true);
  dl->AddRectFilled(p0, p1, IM_COL32(16, 15, 14, 255), 4.f * s);
  // safelight glow, high on the back wall
  ImVec2 bulb(p0.x + kW * s * 0.5f, p0.y + 14.f * s);
  dl->AddCircleFilled(bulb, 80.f * s, IM_COL32(226, 82, 54, 16), 40);
  dl->AddCircleFilled(bulb, 6.f * s, IM_COL32(226, 82, 54, 220), 20);
  auto X = [&](float x) { return p0.x + x * s; };
  auto Y = [&](float h) { return p1.y - (kGround + h) * s; };  // height above the ground -> screen y
  // prints drying on a line (slow parallax)
  {
    float off = std::fmod(r.dist * 0.25f, 120.f);
    float ly = p0.y + 34.f * s;
    dl->AddLine(ImVec2(p0.x, ly), ImVec2(p1.x, ly), IM_COL32(98, 93, 86, 140), 1.f * s);
    for (float x = -off; x < kW + 120; x += 120.f) {
      float px = X(x + 30);
      dl->AddRectFilled(ImVec2(px - 2 * s, ly - 3 * s), ImVec2(px + 2 * s, ly + 3 * s), IM_COL32(152, 145, 133, 200));
      dl->AddRectFilled(ImVec2(px - 13 * s, ly + 3 * s), ImVec2(px + 13 * s, ly + 33 * s), IM_COL32(40, 38, 35, 255), 1.f * s);
      dl->AddRect(ImVec2(px - 13 * s, ly + 3 * s), ImVec2(px + 13 * s, ly + 33 * s), IM_COL32(78, 74, 67, 255), 1.f * s);
    }
  }
  // the floor, moving
  dl->AddLine(ImVec2(p0.x, Y(0)), ImVec2(p1.x, Y(0)), IM_COL32(152, 145, 133, 255), 1.5f * s);
  {
    float off = std::fmod(r.dist, 46.f);
    for (float x = -off; x < kW; x += 46.f)
      dl->AddLine(ImVec2(X(x), Y(-7)), ImVec2(X(x + 14), Y(-7)), IM_COL32(78, 74, 67, 255), 1.f * s);
  }
  // obstacles
  for (const RunObstacle& o : r.obs) {
    ImU32 ink = IM_COL32(200, 194, 182, 255);
    if (o.kind == 0) {  // tripod with a camera on top
      float cx = X(o.x + o.w * 0.5f), top = Y(o.h - 10), base = Y(0);
      dl->AddLine(ImVec2(cx, top), ImVec2(X(o.x), base), ink, 2.f * s);
      dl->AddLine(ImVec2(cx, top), ImVec2(X(o.x + o.w), base), ink, 2.f * s);
      dl->AddLine(ImVec2(cx, top), ImVec2(cx, base), ink, 1.5f * s);
      dl->AddRectFilled(ImVec2(X(o.x + 2), Y(o.h)), ImVec2(X(o.x + o.w - 2), Y(o.h - 11)), ink, 2.f * s);
      dl->AddCircleFilled(ImVec2(X(o.x + o.w * 0.5f), Y(o.h - 5.5f)), 3.f * s, IM_COL32(16, 15, 14, 255));
    } else if (o.kind == 1) {  // film canisters
      int n = std::max(1, (int)(o.w / 16));
      float cw = o.w / n;
      for (int i = 0; i < n; i++) {
        float h = o.h - (i % 2) * 6;
        dl->AddRectFilled(ImVec2(X(o.x + i * cw + 1), Y(h)), ImVec2(X(o.x + (i + 1) * cw - 1), Y(0)), IM_COL32(60, 57, 52, 255), 2.f * s);
        dl->AddRectFilled(ImVec2(X(o.x + i * cw), Y(h)), ImVec2(X(o.x + (i + 1) * cw), Y(h - 5)), ink, 2.f * s);
      }
    } else {  // hanging lamp, glowing red
      float cx = X(o.x + o.w * 0.5f);
      dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, Y(o.lift + o.h)), IM_COL32(98, 93, 86, 255), 1.f * s);
      ImVec2 q[4] = {ImVec2(X(o.x + 9), Y(o.lift + o.h)), ImVec2(X(o.x + o.w - 9), Y(o.lift + o.h)),
                     ImVec2(X(o.x + o.w), Y(o.lift + 4)), ImVec2(X(o.x), Y(o.lift + 4))};
      dl->AddConvexPolyFilled(q, 4, ink);
      dl->AddCircleFilled(ImVec2(cx, Y(o.lift + 3)), 6.f * s, IM_COL32(226, 82, 54, 255), 16);
      dl->AddCircleFilled(ImVec2(cx, Y(o.lift)), 16.f * s, IM_COL32(226, 82, 54, 30), 20);
    }
  }
  // the runner: the Circle Room mark, with a red trail while in the air
  {
    float cx = X(kPx), cy = Y(r.y + kR);
    if (r.y > 2.f)
      for (int i = 1; i <= 4; i++)
        dl->AddCircleFilled(ImVec2(cx - i * 9.f * s, cy + std::min(r.vy, 600.f) * 0.006f * i * s), (5.f - i) * s,
                            IM_COL32(226, 82, 54, 150 - i * 30), 12);
    float squash = r.state == 1 && r.y <= 0.f ? 1.f + 0.04f * std::sin(r.t * 22.f) : 1.f;
    // IC_LOGO's tile spans about 0.6 of the size it is given: 1.65x fills the hit circle
    drawIcon(dl, IC_LOGO, ImVec2(cx, cy + (1.f - squash) * kR * s), kR * 1.65f * s * squash, 0);
  }
  // score
  {
    char sc[64];
    snprintf(sc, sizeof(sc), "%05d", r.score);
    char bs[64];
    snprintf(bs, sizeof(bs), "%s %05d", tr(T_RunBest), std::max(0, r.best));
    ImGui::PushFont(g_fontMono);
    ImVec2 a = ImGui::CalcTextSize(sc), b = ImGui::CalcTextSize(bs);
    dl->AddText(ImVec2(p1.x - a.x - 14 * s, p0.y + 10 * s), pal::kText, sc);
    dl->AddText(ImVec2(p1.x - a.x - b.x - 30 * s, p0.y + 10 * s), pal::kMuted, bs);
    ImGui::PopFont();
  }
  // messages
  if (r.state != 1) {
    const char* msg = r.state == 0 ? tr(T_RunStart) : tr(T_RunOver);
    ImGui::PushFont(g_fontBold);
    ImVec2 ts = ImGui::CalcTextSize(msg);
    ImVec2 at(p0.x + (kW * s - ts.x) * 0.5f, p0.y + kH * s * 0.42f);
    dl->AddRectFilled(ImVec2(at.x - 14 * s, at.y - 8 * s), ImVec2(at.x + ts.x + 14 * s, at.y + ts.y + 8 * s), IM_COL32(16, 15, 14, 220), 4 * s);
    dl->AddText(at, r.state == 2 ? pal::kAccent : pal::kText, msg);
    ImGui::PopFont();
    if (r.state == 2 && r.score > 0 && r.score >= r.best) {
      const char* nb = tr(T_RunNewBest);
      ImVec2 ns = ImGui::CalcTextSize(nb);
      dl->AddText(ImVec2(p0.x + (kW * s - ns.x) * 0.5f, at.y + ts.y + 14 * s), pal::kAccent, nb);
    }
  }
  dl->PopClipRect();
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kFaint);
  ImGui::TextUnformatted(tr(T_RunKeys));
  ImGui::PopStyleColor();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) keep = false;
  if (!keep) {
    ImGui::CloseCurrentPopup();
    r.open = false;
  }
  ImGui::EndPopup();
  return r.open;
}
