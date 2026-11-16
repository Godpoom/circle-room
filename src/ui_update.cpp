#include "ui_update.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "i18n.h"
#include "imgui.h"
#include "platform_win.h"
#include "ui_panels.h"
#include "version.h"

namespace {

const char* kReleases = "https://github.com/Godpoom/circle-room/releases/latest";

// Only our own release pages are opened in the browser.
void openPage(const std::string& url) {
  std::string u = url.rfind("https://github.com/Godpoom/circle-room/", 0) == 0 ? url : std::string(kReleases);
  ShellExecuteW(nullptr, L"open", utf8ToWide(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void muted(const char* s) {
  ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
  ImGui::TextWrapped("%s", s);
  ImGui::PopStyleColor();
}

}  // namespace

std::string updateErrorText(const std::string& e) {
  auto starts = [&](const char* p) { return e.rfind(p, 0) == 0; };
  if (starts("!network")) return tr(T_UpdErrNetwork);
  if (starts("!http")) return std::string(tr(T_UpdErrHttp)) + " (" + e.substr(1) + ")";
  if (starts("!nowrite")) return tr(T_UpdErrNoWrite);
  if (starts("!badhash")) return tr(T_UpdErrHash);
  if (starts("!badzip")) return tr(T_UpdErrZip);
  if (starts("!install")) return tr(T_UpdErrInstall);
  if (starts("!cancelled")) return tr(T_UpdErrCancelled);
  if (starts("!badfeed")) return tr(T_UpdErrFeed);
  if (starts("!noasset")) return tr(T_UpdErrNoAsset);
  return e;
}

void drawUpdateDialog(Updater& u, UpdateHost& h, bool& open) {
  float s = h.scale;
  if (open) {
    ImGui::OpenPopup("###updatedlg");
    open = false;
  }
  ImGui::SetNextWindowSize(ImVec2(500 * s, 0));
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * s, 18 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 6 * s));
  if (!ImGui::BeginPopupModal(trId(T_UpdTitle, "updatedlg"), nullptr, ImGuiWindowFlags_NoResize)) {
    ImGui::PopStyleVar(2);
    return;
  }
  char b[256];
  snprintf(b, sizeof(b), tr(T_UpdCurrent), CR_VERSION_NAME);
  muted(b);
  ImGui::Dummy(ImVec2(0, 2 * s));
  ReleaseInfo r = u.latest();
  std::string err = u.error();
  Updater::State st = u.state();
  bool close = false;
  float bw = 150 * s;

  if (st == Updater::Checking) {
    ImGui::TextUnformatted(tr(T_UpdChecking));
  } else if (st == Updater::UpToDate || st == Updater::Idle) {
    if (st == Updater::UpToDate) ImGui::TextUnformatted(tr(T_UpdUpToDate));
  } else if (st == Updater::Available) {
    ImGui::PushFont(g_fontBold);
    snprintf(b, sizeof(b), tr(T_UpdAvailable), r.version.c_str());
    ImGui::TextUnformatted(b);
    ImGui::PopFont();
    if (r.assetSize > 0) {
      snprintf(b, sizeof(b), tr(T_UpdSize), r.assetSize / 1048576.0);
      muted(b);
    }
    if (!r.notes.empty()) {
      ImGui::Dummy(ImVec2(0, 2 * s));
      ImGui::PushFont(g_fontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, pal::kFaint);
      ImGui::TextUnformatted(tr(T_UpdWhatsNew));
      ImGui::PopStyleColor();
      ImGui::PopFont();
      ImGui::PushStyleColor(ImGuiCol_ChildBg, pal::kCard);
      ImGui::BeginChild("##notes", ImVec2(-FLT_MIN, 200 * s), ImGuiChildFlags_AlwaysUseWindowPadding);
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(r.notes.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
    muted(tr(T_UpdSafe));
  } else if (st == Updater::Downloading || st == Updater::Installing) {
    if (st == Updater::Downloading) {
      snprintf(b, sizeof(b), tr(T_UpdDownloading), u.bytesGot() / 1048576.0, u.bytesTotal() / 1048576.0);
      ImGui::TextUnformatted(b);
      ImGui::ProgressBar(u.progress(), ImVec2(-FLT_MIN, 6 * s), "");
    } else {
      ImGui::TextUnformatted(tr(T_UpdInstalling));
      float t = (float)ImGui::GetTime();
      ImGui::ProgressBar(-1.f * t, ImVec2(-FLT_MIN, 6 * s), "");
    }
  } else if (st == Updater::Ready) {
    snprintf(b, sizeof(b), tr(T_UpdReady), r.version.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kText);
    ImGui::TextWrapped("%s", b);
    ImGui::PopStyleColor();
  } else if (st == Updater::Failed) {
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kAccent);
    ImGui::TextWrapped("%s", updateErrorText(err).c_str());
    ImGui::PopStyleColor();
  }
  if (st == Updater::Available && err == "!noasset") muted(updateErrorText(err).c_str());

  ImGui::Dummy(ImVec2(0, 8 * s));
  // ---- buttons for the state
  if (st == Updater::Available) {
    if (r.assetUrl.empty()) {
      if (uiPrimaryButton(trId(T_UpdOpenPage, "updpage"), ImVec2(bw * 1.3f, 0))) openPage(r.pageUrl);
    } else if (uiPrimaryButton(trId(T_UpdNow, "updnow"), ImVec2(bw, 0))) {
      u.install();
    }
    ImGui::SameLine();
    if (ImGui::Button(trId(T_UpdSkip, "updskip"), ImVec2(bw, 0))) {
      if (h.skip) *h.skip = r.version;
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(trId(T_UpdLater, "updlater"), ImVec2(-FLT_MIN, 0))) close = true;
  } else if (st == Updater::Downloading) {
    if (ImGui::Button(trId(T_Cancel, "updcancel"), ImVec2(bw, 0))) u.cancel();
  } else if (st == Updater::Ready) {
    ImGui::BeginDisabled(!h.canRestart);
    if (uiPrimaryButton(trId(T_UpdRestart, "updrestart"), ImVec2(bw * 1.4f, 0)) && h.restart) h.restart();
    ImGui::EndDisabled();
    if (!h.canRestart && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr(T_UpdRestartBusy));
    ImGui::SameLine();
    if (ImGui::Button(trId(T_UpdLater, "updlater2"), ImVec2(bw, 0))) close = true;
  } else if (st != Updater::Installing && st != Updater::Checking) {
    if (st == Updater::Failed && !r.assetUrl.empty() && compareVersions(r.version, CR_VERSION_STR) > 0) {
      if (uiPrimaryButton(trId(T_UpdRetry, "updretry"), ImVec2(bw, 0))) u.install();
    } else if (uiPrimaryButton(trId(T_UpdCheckAgain, "updcheck"), ImVec2(bw, 0))) {
      u.check(true);
    }
    ImGui::SameLine();
    if (st == Updater::Failed && ImGui::Button(trId(T_UpdOpenPage, "updpage2"), ImVec2(bw * 1.3f, 0))) openPage(r.pageUrl);
    if (st == Updater::Failed) ImGui::SameLine();
    if (ImGui::Button(trId(T_Close, "updclose"), ImVec2(-FLT_MIN, 0))) close = true;
  }

  ImGui::Dummy(ImVec2(0, 6 * s));
  ImGui::Separator();
  ImGui::PushFont(g_fontSmall);
  if (h.autoCheck) ImGui::Checkbox(trId(T_UpdAuto, "updauto"), h.autoCheck);
  muted(tr(T_UpdPrivacy));
  ImGui::PopFont();
  if (close || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    if (st == Updater::Downloading) u.cancel();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  ImGui::PopStyleVar(2);
}
