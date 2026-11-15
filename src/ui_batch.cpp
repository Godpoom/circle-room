#include "ui_batch.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "ai/highlight.h"
#include "ai/ort.h"
#include "ai/style_match.h"
#include "i18n.h"
#include "imgui.h"
#include "platform_win.h"
#include "ui_panels.h"

void AutoBatch::open(std::vector<std::string> photos) {
  if (active_ || photos.empty()) return;
  pending_ = std::move(photos);
  openDlg_ = true;
}

void AutoBatch::runWorker(std::function<void()> fn) {
  ready_ = false;
  th_ = std::thread([this, fn]() {
    try {
      fn();
    } catch (const std::exception& e) {  // out of memory on one photo fails that photo only
      prepOk_ = ok_ = false;
      err_ = e.what();
    }
    ready_ = true;
  });
}

void AutoBatch::shutdown() {
  cancel_ = true;
  if (th_.joinable()) th_.join();
  if (active_) {
    models_.release();
    ai::ortUnload();
  }
  active_ = false;
}

void AutoBatch::start(BatchHost& h) {
  if (pending_.empty()) return;
  if (exportJpg_) {
    outDir_ = pickFolderDialog(h.hwnd, h.lastDir);
    if (outDir_.empty()) return;  // cancelled: nothing starts
  }
  items_ = std::move(pending_);
  pending_.clear();
  pos_ = 0;
  stage_ = 0;
  edited_ = skipped_ = failed_ = exported_ = 0;
  cancel_ = false;
  modelsTried_ = modelsOk_ = false;
  active_ = true;
}

void AutoBatch::finish(BatchHost& h) {
  if (th_.joinable()) th_.join();
  models_.release();
  ai::ortUnload();
  active_ = false;
  char b[256];
  snprintf(b, sizeof(b), tr(T_BatchDone), edited_, skipped_, failed_);
  std::string msg = b;
  if (exportJpg_ && exported_) msg += " \xc2\xb7 " + std::string(tr(T_BatchExported)) + " " + std::to_string(exported_) + ": " + outDir_;
  if (!modelsOk_ && modelsTried_) msg = std::string(tr(T_AiUnavailable)) + ": " + err_;
  h.toast(msg);
}

void AutoBatch::next() {
  pos_++;
  stage_ = 0;
  img_.release();
  std::vector<uint8_t>().swap(rgb_);
}

// Keeps the brush-stroke / magic-eraser lines of a sidecar: serializeParams writes only values.
static std::string extraLines(const std::string& sc) {
  std::stringstream ss(sc);
  std::string line, out;
  while (std::getline(ss, line))
    if (line.compare(0, 6, "stroke") == 0 || line.compare(0, 5, "heal=") == 0) out += line + "\n";
  return out;
}

void AutoBatch::pump(BatchHost& h) {
  if (!active_) return;
  if (th_.joinable()) {
    if (!ready_) return;
    th_.join();
  }
  const std::string path = pos_ < items_.size() ? items_[pos_] : std::string();
  Renderer* rr = h.renderer();
  int maxDim = rr ? rr->maxTextureSize() : 8192;  // the export decodes at most what the GPU can hold
  switch (stage_) {
    case 0: {  // next photo: skip, edit, or only export
      if (cancel_ || pos_ >= items_.size() || (modelsTried_ && !modelsOk_)) {
        finish(h);
        return;
      }
      base_ = EditParams();
      sidecar_.clear();
      bool edited = readSidecar(path, sidecar_);
      if (edited) {
        deserializeParams(sidecar_, base_, true);
        edited = !isDefault(base_);
      }
      extra_ = extraLines(sidecar_);
      params_ = base_;
      if (edited && skipEdited_) {
        skipped_++;
        if (exportJpg_) {  // already edited by hand: exported with those edits
          stage_ = 2;
          runWorker([this, path, maxDim]() { ok_ = loadImage(path, maxDim, img_, err_); });
        } else {
          next();
        }
        return;
      }
      stage_ = 1;
      runWorker([this, path]() {
        prepOk_ = hasEmb_ = segOk_ = false;
        if (!modelsTried_) {
          modelsTried_ = true;
          modelsOk_ = models_.load(err_);
        }
        if (!modelsOk_) return;
        ai::SmallImage small, big;
        if (!ai::decodeScaled(path, 512, small, nullptr, err_)) return;
        hasEmb_ = aiCachedEmbedding(path, emb_);
        if (!hasEmb_ && models_.clip.ok()) {
          std::vector<float> t(ai::kClipTensor);
          ai::Clip::prep(small, t.data());
          hasEmb_ = models_.clip.embed(t.data(), 1, emb_, err_);
        }
        std::string e2;
        seg_ = ai::SegResult();
        if (models_.seg.ok() && ai::decodeScaled(path, 1536, big, nullptr, e2) && models_.seg.run(big, seg_, e2)) {
          ai::makeGuide(big, guide_);
          gw_ = big.w;
          gh_ = big.h;
          segOk_ = true;
        }
        // Blown-out areas: rebuild them now, so the UI thread only reads the cached result.
        if (ai::blownShare(small) > 0.005f) {
          ai::Recovery rec;
          ai::recoverFile(path, segOk_ ? &seg_ : nullptr, true, rec, e2);
        }
        prepOk_ = true;
      });
      return;
    }
    case 1: {  // AI ready: the GPU solve (UI thread, a fraction of a second)
      Renderer* r = h.renderer();
      ai::AutoResult res;
      std::string err;
      if (!prepOk_ || !r ||
          !aiAutoEditFile(*r, models_, path, hasEmb_ ? emb_ : nullptr, segOk_ ? &seg_ : nullptr, segOk_ ? &guide_ : nullptr,
                          gw_, gh_, segOk_, base_, res, err, true)) {
        if (!modelsOk_) {  // stage 0 ends the batch and reports why
          stage_ = 0;
          return;
        }
        failed_++;
        next();
        return;
      }
      params_ = ai::blendStyle(base_, res.params, strength_ / 100.f);
      copyLayers(params_, res.params);  // region layers and highlight rebuild come with the result
      std::memcpy(params_.img, base_.img, sizeof(params_.img));  // photo layers and erasures stay
      params_.heals = base_.heals;
      sidecar_ = serializeParams(params_, true) + extra_;
      writeSidecar(path, sidecar_);
      edited_++;
      if (h.edited) h.edited(path);
      if (!exportJpg_) {
        next();
        return;
      }
      stage_ = 2;
      runWorker([this, path, maxDim]() { ok_ = loadImage(path, maxDim, img_, err_); });
      return;
    }
    case 2: {  // full-size render (UI thread), then encode on the worker
      Renderer* r = h.renderer();
      if (!ok_ || !r || !renderEdited(*r, path, params_, sidecar_, img_, rgb_, w_, h_, err_)) {
        failed_++;
        next();
        return;
      }
      out_ = freePath(outDir_, fileStem(path), ".jpg");
      stage_ = 3;
      runWorker([this]() {
        ok_ = writeImage(out_, rgb_.data(), w_, h_, false, 92, err_);
        std::vector<uint8_t>().swap(rgb_);
      });
      return;
    }
    case 3:
      if (ok_) exported_++;
      else failed_++;
      next();
      return;
  }
}

void AutoBatch::draw(BatchHost& h) {
  float s = h.scale;
  // ---- options dialog
  if (openDlg_) {
    ImGui::OpenPopup("###autobatch");
    openDlg_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560 * s, 0));
  if (ImGui::BeginPopupModal(trId(T_BatchTitle, "autobatch"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    char b[160];
    snprintf(b, sizeof(b), tr(T_BatchCount), (int)pending_.size());
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted(b);
    ImGui::PopFont();
    uiHint(tr(T_BatchHint));
    ImGui::Dummy(ImVec2(0, 6 * s));
    EditResult dummy;
    uiSlider(dummy, T_Strength, "bstr", &strength_, 0, 150, 100, "%.0f%%");
    ImGui::Checkbox(trId(T_BatchSkip, "bskip"), &skipEdited_);
    ImGui::Indent(30 * s);
    uiHint(tr(T_BatchSkipHint));
    ImGui::Unindent(30 * s);
    ImGui::Checkbox(trId(T_BatchExport, "bexp"), &exportJpg_);
    ImGui::Indent(30 * s);
    uiHint(tr(T_BatchExportHint));
    ImGui::Unindent(30 * s);
    ImGui::Dummy(ImVec2(0, 8 * s));
    if (uiPrimaryButton(trId(T_BatchStart, "bgo"), ImVec2(260 * s, 0))) {
      ImGui::CloseCurrentPopup();
      start(h);
    }
    ImGui::SameLine();
    if (ImGui::Button(trId(T_Cancel, "bcancel"))) {
      pending_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  // ---- progress card, bottom right over everything
  if (!active_) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20 * s, vp->WorkPos.y + vp->WorkSize.y - 44 * s),
                          ImGuiCond_Always, ImVec2(1, 1));
  ImGui::SetNextWindowSize(ImVec2(400 * s, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 27, 25, 245));
  ImGui::PushStyleColor(ImGuiCol_Border, pal::kAccent);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 12 * s));
  ImGui::Begin("##batchprogress", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
  int n = (int)items_.size(), at = (int)std::min(pos_ + 1, items_.size());
  char b[160];
  snprintf(b, sizeof(b), tr(T_BatchProgress), at, n);
  ImGui::PushFont(g_fontBold);
  ImGui::TextUnformatted(b);
  ImGui::PopFont();
  if (pos_ < items_.size()) {
    const char* what = stage_ == 1 ? tr(T_BatchAiStage) : stage_ >= 2 ? tr(T_BatchSaveStage) : "";
    std::string line = fileName(items_[pos_]) + (what[0] ? "  \xc2\xb7  " + std::string(what) : "");
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopStyleColor();
  }
  float frac = n ? (pos_ + (stage_ == 0 ? 0.f : stage_ == 1 ? .4f : stage_ == 2 ? .7f : .9f)) / n : 1.f;
  ImGui::ProgressBar(std::min(1.f, frac), ImVec2(-FLT_MIN, 6 * s), "");
  ImGui::BeginDisabled(cancel_);
  if (ImGui::SmallButton(trId(T_Cancel, "bstop"))) cancel_ = true;
  ImGui::EndDisabled();
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}
