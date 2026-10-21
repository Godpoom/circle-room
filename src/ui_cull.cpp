#include "ui_cull.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gl_loader.h"
#include "i18n.h"
#include "image_io.h"
#include "imgui.h"
#include "platform_win.h"
#include "ui_icons.h"
#include "ui_panels.h"

// ---------------------------------------------------------------- shared helpers
bool aiCachedEmbedding(const std::string& path, float* emb) {
  std::vector<ai::PhotoInfo> c;
  if (!ai::loadCache(dirOf(path), c)) return false;
  std::string name = fileName(path);
  for (const auto& p : c)
    if (fileName(p.path) == name && p.hasAi) {
      std::memcpy(emb, p.emb, sizeof(p.emb));
      return true;
    }
  return false;
}

bool aiStyleSolveFile(Renderer& r, const ai::StyleSet& set, const std::string& path, const float* emb,
                      const EditParams& base, unsigned groups, ai::StyleResult& out, std::string& err) {
  ai::SmallImage si;
  if (!ai::decodeScaled(path, 512, si, nullptr, err)) return false;
  ImageData d;
  d.w = si.w;
  d.h = si.h;
  d.px8.swap(si.rgb);
  if (!r.upload(d, err)) return false;
  out = ai::solveStyle(r, set, emb, base, groups);
  return true;
}

static const int kSceneKeys[ai::SC_COUNT] = {T_Sc_Portrait, T_Sc_Group, T_Sc_Landscape, T_Sc_Sunset,
                                             T_Sc_Night,    T_Sc_Indoor, T_Sc_Food,     T_Sc_Beach,
                                             T_Sc_Snow,     T_Sc_Forest, T_Sc_City,     T_Sc_Animal};
const char* sceneName(int s) { return s >= 0 && s < ai::SC_COUNT ? tr(kSceneKeys[s]) : "-"; }

bool AutoModels::load(std::string& err) {
  if (!clip.ok() && !clip.load(false, err)) return false;
  std::string serr;
  if (!seg.ok()) seg.load(serr);  // without it auto edit just skips local layers
  return true;
}

void AutoModels::release() {
  clip.release();
  seg.release();
}

bool aiAutoEditFile(Renderer& r, AutoModels& m, const std::string& path, const float* emb, const ai::SegResult* seg,
                    const std::vector<float>* guide, int gw, int gh, bool wantLayers, const EditParams& base,
                    ai::AutoResult& out, std::string& err, bool recover) {
  ai::SmallImage si;
  if (!ai::decodeScaled(path, 512, si, nullptr, err)) return false;
  float blown = ai::blownShare(si);
  float own[ai::kEmbDim];
  if (!emb && m.clip.ok()) {  // scene needs an embedding: one CLIP run on the small image
    std::vector<float> t(ai::kClipTensor);
    ai::Clip::prep(si, t.data());
    if (m.clip.embed(t.data(), 1, own, err)) emb = own;
  }
  ai::SegResult localSeg;
  std::vector<float> localGuide;
  if (wantLayers && !seg && m.seg.ok()) {
    ai::SmallImage big;
    std::string e2;
    if (ai::decodeScaled(path, 1536, big, nullptr, e2) && m.seg.run(big, localSeg, e2)) {
      ai::makeGuide(big, localGuide);
      seg = &localSeg;
      guide = &localGuide;
      gw = big.w;
      gh = big.h;
    }
  }
  ImageData d;
  d.w = si.w;
  d.h = si.h;
  d.px8.swap(si.rgb);
  if (!r.upload(d, err)) return false;
  ai::AutoInputs in;
  in.emb = emb;
  in.clip = m.clip.ok() ? &m.clip : nullptr;
  if (wantLayers) {
    in.seg = seg;
    in.guide = guide;
    in.guideW = gw;
    in.guideH = gh;
  }
  out = ai::autoEdit(r, in, base);
  if (recover && blown > 0.005f) {  // pure-white areas: rebuild them (cached next to the photo)
    ai::Recovery rec;
    std::string e2;
    if (ai::recoverFile(path, seg, true, rec, e2) && rec.areas > 0) out.params.recover = 100.f;
  }
  return true;
}

void StyleLibrary::reload() {
  sets_.clear();
  for (const std::string& f : ai::listStyleFiles()) {
    ai::StyleSet s;
    if (s.load(f)) sets_.push_back(std::move(s));
  }
  std::sort(sets_.begin(), sets_.end(), [](const ai::StyleSet& a, const ai::StyleSet& b) { return a.name < b.name; });
  selected = std::clamp(selected, 0, std::max(0, (int)sets_.size() - 1));
}

bool StyleLibrary::save(const ai::StyleSet& s, std::string& err) {
  std::string name = s.name;
  for (char& ch : name)
    if (strchr("\\/:*?\"<>|", ch)) ch = '_';
  if (name.empty()) name = "style";
  if (!s.save(ai::stylesDir() + "\\" + name + ".lrs")) {
    err = "cannot write style";
    return false;
  }
  reload();
  for (size_t i = 0; i < sets_.size(); i++)
    if (sets_[i].name == s.name) selected = (int)i;
  return true;
}

void StyleLibrary::startCreate(const std::string& name, const std::vector<std::string>& files) {
  pendingName_ = name;
  an_.startFiles(files, false);
}

bool StyleLibrary::pump(std::string& err) {
  if (!an_.poll()) return false;
  std::vector<ai::PhotoInfo> refs = an_.takeResults();
  if (refs.size() < 5) {
    err = tr(T_NeedRefs);
    return false;
  }
  return save(ai::StyleSet::fromPhotos(pendingName_, refs), err);
}

// ---------------------------------------------------------------- CullView
static const int kReasonKeys[ai::R_COUNT_BITS] = {T_R_Blurry, T_R_FaceBlur, T_R_EyesClosed, T_R_Over,     T_R_Under,
                                                  T_R_Noisy,  T_R_Tilted,   T_R_EdgeCut,    T_R_Balance,  T_R_GoodComp,
                                                  T_R_Sharp,  T_R_Light,    T_R_Dup,        T_R_LikeRef};
static const uint32_t kGoodReasons = ai::R_GOOD_COMP | ai::R_SHARP | ai::R_GOOD_LIGHT | ai::R_LIKE_REF;

void CullView::shutdown() {
  if (searchTh_.joinable()) searchTh_.join();
  textQ_.release();
  an_.cancel();
  refAn_.cancel();
  an_.wait();
  refAn_.wait();
  clearThumbs();
}

bool CullView::busy() const {
  return an_.running() || refAn_.running() || batchActive_ || wantFrames_ || searchRunning_ || textQ_.ok();
}

void CullView::startSearch(const std::string& q) {
  if (searchRunning_ || q.empty()) return;
  if (searchTh_.joinable()) searchTh_.join();
  pendingQuery_ = q;
  searchRunning_ = true;
  searchDone_ = false;
  searchTh_ = std::thread([this] {
    std::string err;
    searchOk_ = (textQ_.ok() || textQ_.load(err)) && textQ_.embed(pendingQuery_, pendingEmb_, &searchEnglish_, err);
    searchErr_ = err;
    searchDone_ = true;
  });
}

// Sorts by cosine to qEmb_; strictness keeps only clearly-above-average matches.
void CullView::applySearch() {
  std::vector<std::pair<float, int>> s;
  double mean = 0, var = 0;
  for (size_t i = 0; i < photos_.size(); i++) {
    if (!photos_[i].hasAi) continue;
    float d = 0;
    for (int k = 0; k < ai::kEmbDim; k++) d += qEmb_[k] * photos_[i].emb[k];
    s.push_back({d, (int)i});
    mean += d;
  }
  if (s.empty()) {
    ranked_.clear();
    searchMatches_ = 0;
    return;
  }
  mean /= s.size();
  for (auto& x : s) var += (x.first - mean) * (x.first - mean);
  float sd = (float)std::sqrt(var / s.size()) + 1e-6f;
  std::sort(s.rbegin(), s.rend());
  float top = s[0].first, thr = strict_ / 100.f * 2.5f;  // z-score cut-off
  std::vector<int> pos(photos_.size(), -1);
  for (size_t i = 0; i < allRanked_.size(); i++) pos[allRanked_[i].idx] = (int)i;
  ranked_.clear();
  for (auto& x : s) {
    if ((x.first - mean) / sd < thr && !ranked_.empty()) break;
    ai::RankEntry e = pos[x.second] >= 0 ? allRanked_[pos[x.second]] : ai::RankEntry();
    e.idx = x.second;
    e.score = std::round(100.f * std::clamp((x.first - (float)mean) / std::max(1e-6f, top - (float)mean), 0.f, 1.f));
    ranked_.push_back(e);
  }
  searchMatches_ = (int)ranked_.size();
  if (selected_ >= (int)ranked_.size()) selected_ = ranked_.empty() ? -1 : 0;
}

void CullView::clearThumbs() {
  for (auto& kv : thumbs_)
    if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
  thumbs_.clear();
}

void CullView::setFolder(const std::string& f) {
  if (f.empty() || an_.running()) return;
  folder_ = f;
  photos_.clear();
  ranked_.clear();
  selected_ = -1;
  clearThumbs();
  an_.startFolder(f, false);
}

void CullView::addRefs(const std::vector<std::string>& files) {
  for (const std::string& f : files)
    if (std::find(refPaths_.begin(), refPaths_.end(), f) == refPaths_.end()) refPaths_.push_back(f);
  if (!refAn_.running() && !refPaths_.empty()) {
    refsSubmitted_ = refPaths_.size();
    refAn_.startFiles(refPaths_, false);
  }
  rs_.refMode = true;
}

void CullView::onDrop(const std::vector<std::string>& paths) {
  if (paths.size() == 1 && isDirectory(paths[0])) {
    setFolder(paths[0]);
    return;
  }
  std::vector<std::string> imgs;
  for (const std::string& p : paths)
    if (!isDirectory(p)) imgs.push_back(p);
  if (!imgs.empty()) addRefs(imgs);  // dropped photos become references
}

int CullView::shownCount() const { return showAll_ ? (int)ranked_.size() : std::min(topN_, (int)ranked_.size()); }

void CullView::pump(CullHost& h, StyleLibrary& lib) {
  frame_++;
  loadsThisFrame_ = 0;
  wantFrames_ = false;
  if (an_.poll()) {
    clearThumbs();
    photos_ = an_.takeResults();
    groups_ = ai::groupDuplicates(photos_);
    device_ = an_.message();
    rankDirty_ = true;
    selected_ = photos_.empty() ? -1 : 0;
  }
  if (refAn_.poll()) {
    refs_ = refAn_.takeResults();
    rankDirty_ = true;
    // References added while the job ran
    if (refPaths_.size() > refsSubmitted_) {
      refsSubmitted_ = refPaths_.size();
      refAn_.startFiles(refPaths_, false);
    }
  }
  if (searchRunning_ && searchDone_) {
    searchTh_.join();
    searchRunning_ = false;
    lastSearch_ = std::chrono::steady_clock::now();
    if (searchOk_) {
      std::memcpy(qEmb_, pendingEmb_, sizeof(qEmb_));
      searchLabel_ = pendingQuery_;
      searchActive_ = true;
      selected_ = 0;
      rankDirty_ = true;
    } else {
      h.toast(std::string(tr(T_AiUnavailable)) + ": " + searchErr_);
    }
  }
  // The text models (~170 MB) leave RAM after a minute without searching.
  if (!searchRunning_ && textQ_.ok() && std::chrono::steady_clock::now() - lastSearch_ > std::chrono::seconds(60)) {
    textQ_.release();
    ai::ortUnload();
  }
  if (rankDirty_) {
    bool useRefs = rs_.refMode && refs_.size() >= 5;
    ai::RankSettings s = rs_;
    s.refMode = useRefs;
    qualityRanked_ = ai::rankPhotos(photos_, groups_, refs_, s);
    s.dedupe = false;
    allRanked_ = ai::rankPhotos(photos_, groups_, refs_, s);
    rankDirty_ = false;
    if (searchActive_) applySearch();
    else ranked_ = qualityRanked_;
    if (selected_ >= (int)ranked_.size()) selected_ = ranked_.empty() ? -1 : 0;
  }
  if (batchActive_) {  // one photo per frame: decode small + GPU solve (~0.5 s)
    if (batchPos_ >= batchItems_.size()) {
      batchActive_ = false;
      if (batchAuto_) {
        autoModels_.release();
        ai::ortUnload();
      }
      h.toast(std::string(tr(T_Styled)) + " " + std::to_string(batchItems_.size()) + " " + tr(T_PhotosUnit));
    } else if (batchAuto_) {
      const ai::PhotoInfo& p = photos_[batchItems_[batchPos_++]];
      Renderer* r = h.aiRenderer();
      EditParams base;
      std::string sc, err;
      if (readSidecar(p.path, sc)) deserializeParams(sc, base, true);
      ai::AutoResult res;
      if (r && aiAutoEditFile(*r, autoModels_, p.path, p.hasAi ? p.emb : nullptr, nullptr, nullptr, 0, 0, true, base, res,
                              err, true))
        writeSidecar(p.path, serializeParams(res.params, true));
    } else {
      const ai::PhotoInfo& p = photos_[batchItems_[batchPos_++]];
      Renderer* r = h.aiRenderer();
      EditParams base;
      std::string sc, err;
      if (readSidecar(p.path, sc)) deserializeParams(sc, base, true);
      ai::StyleResult res;
      if (r && aiStyleSolveFile(*r, batchSet_, p.path, p.hasAi ? p.emb : nullptr, base, ai::SG_ALL, res, err)) {
        EditParams out = ai::blendStyle(base, res.params, batchStrength_ / 100.f);
        writeSidecar(p.path, serializeParams(out, true));
      }
    }
  }
  (void)lib;
}

unsigned CullView::thumb(int photo, int& w, int& h) {
  auto it = thumbs_.find(photo);
  if (it != thumbs_.end()) {
    it->second.lastFrame = frame_;
    w = it->second.w;
    h = it->second.h;
    return it->second.tex;
  }
  if (loadsThisFrame_ >= 12) {  // spread decoding over frames
    wantFrames_ = true;
    return 0;
  }
  loadsThisFrame_++;
  const ai::PhotoInfo& p = photos_[photo];
  std::vector<uint8_t> jpg, rgb;
  Thumb t;
  t.lastFrame = frame_;
  if (ai::readThumb(folder_, p.thumbOff, p.thumbLen, jpg) && decodeRgbMem(jpg.data(), jpg.size(), rgb, t.w, t.h)) {
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  if (thumbs_.size() > 300) {  // keep VRAM small: drop thumbnails not drawn recently
    for (auto i = thumbs_.begin(); i != thumbs_.end();) {
      if (i->second.lastFrame < frame_ - 2) {
        if (i->second.tex) glDeleteTextures(1, &i->second.tex);
        i = thumbs_.erase(i);
      } else {
        ++i;
      }
    }
  }
  thumbs_[photo] = t;
  w = t.w;
  h = t.h;
  return t.tex;
}

static bool weightSlider(int key, const char* id, float* v, float def) {
  EditResult r;
  return uiSlider(r, key, id, v, 0, 100, def, "%.0f") || r.changed;
}

void CullView::drawControls(CullHost& h, StyleLibrary& lib) {
  float s = h.scale;
  uiSectionReset();
  uiCardBegin("##cfolder", tr(T_ModeCull), false);
  if (uiPrimaryButton(trId(T_ChooseFolder, "folder"))) setFolder(pickFolderDialog(h.hwnd, folder_));
  tourSetItem(TR_CFOLDER);
  if (!folder_.empty()) uiHint(folder_.c_str());
  if (an_.running()) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %d/%d", tr(T_Analyzing), an_.done(), an_.total());
    ImGui::ProgressBar(an_.progress(), ImVec2(-FLT_MIN, 0), buf);
    if (ImGui::Button(trId(T_Cancel, "cancelan"))) an_.cancel();
  } else if (!folder_.empty()) {
    if (ImGui::Button(trId(T_Analyze, "rescan"))) setFolder(folder_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu %s", photos_.size(), tr(T_PhotosUnit));
  }
  if (!device_.empty()) uiHint((std::string(tr(T_Device)) + ": " + device_).c_str());
  uiCardEnd();

  uiCardBegin("##crank", tr(T_RankBy), false);
  const char* rmodes[2] = {tr(T_ModeQuality), tr(T_ModeReference)};
  int rm = uiSegmented("rankmode", rmodes, 2, rs_.refMode ? 1 : 0);
  if (rm >= 0 && (rm == 1) != rs_.refMode) rs_.refMode = rm == 1, rankDirty_ = true;
  if (rs_.refMode) {
    ImGui::Text("%s: %zu", tr(T_References), refPaths_.size());
    if (refAn_.running()) ImGui::ProgressBar(refAn_.progress(), ImVec2(-FLT_MIN, 0), tr(T_Analyzing));
    int removeAt = -1;
    ImGui::BeginChild("##refs", ImVec2(0, std::min(6.f, (float)refPaths_.size()) * ImGui::GetFrameHeightWithSpacing() + 4),
                      ImGuiChildFlags_None);
    for (size_t i = 0; i < refPaths_.size(); i++) {
      ImGui::PushID((int)i);
      if (ImGui::SmallButton("×")) removeAt = (int)i;
      ImGui::SameLine();
      ImGui::TextUnformatted(fileName(refPaths_[i]).c_str());
      ImGui::PopID();
    }
    ImGui::EndChild();
    if (removeAt >= 0) {
      refPaths_.erase(refPaths_.begin() + removeAt);
      refs_.erase(std::remove_if(refs_.begin(), refs_.end(),
                                 [&](const ai::PhotoInfo& p) {
                                   return std::find(refPaths_.begin(), refPaths_.end(), p.path) == refPaths_.end();
                                 }),
                  refs_.end());
      rankDirty_ = true;
    }
    if (ImGui::Button(trId(T_AddRefs, "addrefs"))) addRefs(openImagesDialog(h.hwnd, folder_));
    ImGui::SameLine();
    if (ImGui::Button(trId(T_Clear, "clearrefs"))) {
      refPaths_.clear();
      refs_.clear();
      rankDirty_ = true;
    }
    if (refs_.size() < 5) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal::kAccent), "%s", tr(T_NeedRefs));
    ImGui::BeginDisabled(refs_.size() < 5 || refAn_.running());
    if (ImGui::Button(trId(T_SaveStyle, "savestyle"))) {
      openSaveStyle_ = true;
      snprintf(nameBuf_, sizeof(nameBuf_), "Style %d", (int)lib.sets().size() + 1);
    }
    ImGui::EndDisabled();
    if (weightSlider(T_QualityVsSim, "alpha", &rs_.alpha, 50)) rankDirty_ = true;
  }
  uiCardEnd();

  uiCardBegin("##ctop", tr(T_ShowTop), false);
  ImGui::SetNextItemWidth(96 * s);
  if (ImGui::InputInt("##topn", &topN_, 1, 10)) topN_ = std::max(1, topN_), showAll_ = false;
  for (int n : {10, 50, 100}) {
    ImGui::SameLine();
    char b[8];
    snprintf(b, sizeof(b), "%d", n);
    if (uiPill(b, !showAll_ && topN_ == n)) topN_ = n, showAll_ = false;
  }
  ImGui::SameLine();
  if (uiPill(tr(T_All), showAll_)) showAll_ = true;
  if (ImGui::Checkbox(trId(T_Dedupe, "dedupe"), &rs_.dedupe)) rankDirty_ = true;
  uiCardEnd();
  uiCardBegin("##cweights", tr(T_Weights), false);
  if (weightSlider(T_Technical, "wt", &rs_.wTech, 30)) rankDirty_ = true;
  if (weightSlider(T_Aesthetic, "wa", &rs_.wAes, 40)) rankDirty_ = true;
  if (weightSlider(T_Composition, "wc", &rs_.wComp, 30)) rankDirty_ = true;
  uiCardEnd();

  uiCardBegin("##cactions", tr(T_ActOnShown), false);
  int shown = shownCount();
  ImGui::BeginDisabled(shown == 0 || batchActive_);
  if (ImGui::Button(trId(T_CopyTop, "copytop"), ImVec2(-FLT_MIN, 0))) {
    std::string dst = pickFolderDialog(h.hwnd, folder_);
    if (!dst.empty() && dst != folder_) {
      int n = 0;
      for (int i = 0; i < shown; i++) {
        const std::string& src = photos_[ranked_[i].idx].path;
        if (!copyFileU(src, dst + "\\" + fileName(src))) continue;
        n++;
        std::string sc;
        if (readSidecar(src, sc)) writeTextFile(sidecarPath(dst + "\\" + fileName(src)), sc);
      }
      h.toast(std::string(tr(T_Copied)) + " " + std::to_string(n) + " " + tr(T_PhotosUnit));
    }
  }
  // Style source for batch editing: current references or a saved style.
  bool haveRefs = refs_.size() >= 5;
  std::vector<std::string> names;
  if (haveRefs) names.push_back(std::string(tr(T_References)) + " (" + std::to_string(refs_.size()) + ")");
  for (const auto& st : lib.sets()) names.push_back(st.name);
  if (!names.empty()) {
    int cur = batchSource_ < 0 ? 0 : batchSource_ + (haveRefs ? 1 : 0);
    if (!haveRefs && batchSource_ < 0) cur = 0;
    cur = std::clamp(cur, 0, (int)names.size() - 1);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kMuted);
    ImGui::TextUnformatted(tr(T_Style));
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##batchstyle", names[cur].c_str())) {
      for (int i = 0; i < (int)names.size(); i++)
        if (ImGui::Selectable(names[i].c_str(), i == cur)) cur = i;
      ImGui::EndCombo();
    }
    batchSource_ = haveRefs ? cur - 1 : cur;
    weightSlider(T_Strength, "bstr", &batchStrength_, 80);
    if (ImGui::Button(trId(T_StyleTop, "styletop"), ImVec2(-FLT_MIN, 0))) {
      batchAuto_ = false;
      batchSet_ = batchSource_ < 0 ? ai::StyleSet::fromPhotos("refs", refs_) : lib.sets()[batchSource_];
      batchItems_.clear();
      for (int i = 0; i < shown; i++) batchItems_.push_back(ranked_[i].idx);
      batchPos_ = 0;
      batchActive_ = !batchSet_.refs.empty();
    }
  }
  if (ImGui::Button(trId(T_AutoTop, "autotop"), ImVec2(-FLT_MIN, 0))) {
    std::string err;
    if (autoModels_.load(err)) {
      batchAuto_ = true;
      batchItems_.clear();
      for (int i = 0; i < shown; i++) batchItems_.push_back(ranked_[i].idx);
      batchPos_ = 0;
      batchActive_ = true;
    } else {
      h.toast(std::string(tr(T_AiUnavailable)) + ": " + err);
    }
  }
  ImGui::EndDisabled();
  if (batchActive_) {
    ImGui::ProgressBar(batchItems_.empty() ? 1.f : (float)batchPos_ / batchItems_.size(), ImVec2(-FLT_MIN, 0),
                       tr(T_Working));
    if (ImGui::Button(trId(T_Cancel, "cancelbatch"))) batchActive_ = false;
  }
  uiCardEnd();

  if (openSaveStyle_) {
    ImGui::OpenPopup("###savestyle");
    openSaveStyle_ = false;
  }
  if (ImGui::BeginPopupModal(trId(T_SaveStyle, "savestyle"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(tr(T_StyleName));
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##sname", nameBuf_, sizeof(nameBuf_), ImGuiInputTextFlags_EnterReturnsTrue);
    if ((uiPrimaryButton(trId(T_Save, "ssave"), ImVec2(120 * s, 0)) || enter) && nameBuf_[0]) {
      std::string err;
      if (lib.save(ai::StyleSet::fromPhotos(nameBuf_, refs_), err)) h.toast(std::string(tr(T_Saved)) + ": " + nameBuf_);
      else h.toast(err);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(trId(T_Cancel, "scancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void CullView::drawGrid(CullHost& h) {
  float s = h.scale;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  int count = shownCount();
  if (count == 0) {
    ImVec2 c0 = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
    if (folder_.empty() && !an_.running()) {
      // First visit: a three-step guide and a big button, so the way in is obvious.
      const char* steps[3] = {tr(T_CullGuide1), tr(T_CullGuide2), tr(T_CullGuide3)};
      ImGui::PushFont(g_fontTitle);
      const char* head = tr(T_CullGuideT);
      ImVec2 hs = ImGui::CalcTextSize(head);
      ImGui::PopFont();
      float rowH = 40 * s, blockH = hs.y + 28 * s + rowH * 3 + 24 * s + ImGui::GetFrameHeight() * 1.4f;
      float x0 = c0.x + sz.x * 0.5f - 210 * s, y = c0.y + std::max(20.f * s, (sz.y - blockH) * 0.45f);
      ImGui::PushFont(g_fontTitle);
      dl->AddText(ImVec2(x0, y), pal::kText, head);
      ImGui::PopFont();
      y += hs.y + 28 * s;
      for (int i = 0; i < 3; i++) {
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        ImVec2 cc(x0 + 14 * s, y + rowH * 0.5f);
        dl->AddCircle(cc, 14 * s, pal::kAccent, 24, 1.5f);
        ImGui::PushFont(g_fontMono);
        ImVec2 ns = ImGui::CalcTextSize(num);
        dl->AddText(ImVec2(cc.x - ns.x * 0.5f, cc.y - ns.y * 0.5f), pal::kAccent, num);
        ImGui::PopFont();
        dl->AddText(ImVec2(x0 + 40 * s, y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), i == 0 ? pal::kText : pal::kMuted, steps[i]);
        if (i < 2) dl->AddLine(ImVec2(cc.x, cc.y + 16 * s), ImVec2(cc.x, cc.y + rowH - 16 * s), pal::kFrame, 1.f);
        y += rowH;
      }
      ImGui::SetCursorScreenPos(ImVec2(x0, y + 16 * s));
      if (uiPrimaryButton(trId(T_ChooseFolder, "bigfolder"), ImVec2(260 * s, ImGui::GetFrameHeight() * 1.4f)))
        setFolder(pickFolderDialog(h.hwnd, folder_));
      return;
    }
    const char* msg = an_.running() ? tr(T_Analyzing) : tr(T_NoFolder);
    ImVec2 ts = ImGui::CalcTextSize(msg);
    drawIcon(dl, IC_GRID, ImVec2(c0.x + sz.x / 2, c0.y + sz.y / 2 - 40 * s), 40 * s, pal::kFaint);
    dl->AddText(ImVec2(c0.x + (sz.x - ts.x) / 2, c0.y + (sz.y - ts.y) / 2), pal::kMuted, msg);
    return;
  }
  float cell = 178.f * s, labelH = ImGui::GetTextLineHeight() * 2 + 8 * s;
  float avail = ImGui::GetContentRegionAvail().x;
  int cols = std::max(1, (int)(avail / cell));
  cell = avail / cols;
  int rows = (count + cols - 1) / cols;
  ImGuiListClipper clip;
  clip.Begin(rows, cell + labelH);
  while (clip.Step()) {
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
      for (int c = 0; c < cols; c++) {
        int i = r * cols + c;
        if (i >= count) break;
        const ai::RankEntry& e = ranked_[i];
        const ai::PhotoInfo& p = photos_[e.idx];
        if (c) ImGui::SameLine(0, 0);
        ImGui::PushID(i);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##cell", ImVec2(cell, cell + labelH));
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) selected_ = i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) h.openInEditor(p.path);
        bool sel = i == selected_;
        ImVec2 a(p0.x + 6 * s, p0.y + 6 * s), b(p0.x + cell - 6 * s, p0.y + cell - 6 * s);
        dl->AddRectFilled(a, b, sel ? pal::kFrame : (hov ? pal::kHover : pal::kCard), 2.f * s);
        int tw = 0, th = 0;
        unsigned tex = thumb(e.idx, tw, th);
        if (tex && tw > 0 && th > 0) {
          float bw = b.x - a.x - 8 * s, bh = b.y - a.y - 8 * s, k = std::min(bw / tw, bh / th);
          float iw = tw * k, ih = th * k, ix = (a.x + b.x - iw) / 2, iy = (a.y + b.y - ih) / 2;
          dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(ix, iy), ImVec2(ix + iw, iy + ih));
        }
        if (sel) dl->AddRect(a, b, pal::kAccent, 2.f * s, 0, 1.5f);
        char badge[32];
        snprintf(badge, sizeof(badge), "#%d  %.0f", i + 1, e.score);
        ImVec2 bs = ImGui::CalcTextSize(badge);
        bool top3 = i < 3;
        dl->AddRectFilled(ImVec2(a.x + 6, a.y + 6), ImVec2(a.x + bs.x + 18, a.y + bs.y + 10),
                          top3 ? pal::kAccent : IM_COL32(14, 13, 12, 210), 2.f);
        dl->AddText(ImVec2(a.x + 12, a.y + 8), top3 ? pal::kOnAccent : pal::kText, badge);
        if (e.groupSize > 1) {
          char g[16];
          snprintf(g, sizeof(g), "x%d", e.groupSize);
          ImVec2 gs = ImGui::CalcTextSize(g);
          dl->AddRectFilled(ImVec2(b.x - gs.x - 18, a.y + 6), ImVec2(b.x - 6, a.y + gs.y + 10), IM_COL32(14, 13, 12, 210),
                            2.f);
          dl->AddText(ImVec2(b.x - gs.x - 12, a.y + 8), pal::kAccent, g);
        }
        // Label: file name + first reason
        float ly = p0.y + cell;
        std::string name = fileName(p.path);
        dl->PushClipRect(ImVec2(p0.x + 6 * s, ly), ImVec2(p0.x + cell - 6 * s, ly + labelH), true);
        dl->AddText(ImVec2(p0.x + 10 * s, ly), sel ? pal::kText : pal::kMuted, name.c_str());
        uint32_t bad = e.reasons & ~kGoodReasons & ~(uint32_t)ai::R_DUPLICATE, good = e.reasons & kGoodReasons;
        uint32_t show = bad ? bad : good;
        for (int k = 0; k < (int)ai::R_COUNT_BITS; k++)
          if (show & (1u << k)) {
            dl->AddText(ImVec2(p0.x + 10 * s, ly + ImGui::GetTextLineHeight() + 2),
                        bad ? IM_COL32(255, 150, 120, 255) : IM_COL32(140, 220, 140, 255), tr(kReasonKeys[k]));
            break;
          }
        dl->PopClipRect();
        ImGui::PopID();
      }
    }
  }
}

void CullView::drawDetail(CullHost& h) {
  if (selected_ < 0 || selected_ >= (int)ranked_.size()) return;
  float s = h.scale;
  const ai::RankEntry& e = ranked_[selected_];
  const ai::PhotoInfo& p = photos_[e.idx];
  int tw = 0, th = 0;
  unsigned tex = thumb(e.idx, tw, th);
  float w = ImGui::GetContentRegionAvail().x;
  if (tex && tw > 0) {
    // Cap the preview height: a tall portrait used to overflow the panel, the scrollbar then
    // narrowed it, the photo shrank and fitted, the scrollbar went away, and it looped every frame.
    float maxH = ImGui::GetWindowHeight() * 0.48f;
    float iw = w, ih = w * th / tw;
    if (ih > maxH) ih = maxH, iw = ih * tw / th;
    ImVec2 q = ImGui::GetCursorScreenPos();
    q.x += std::floor((w - iw) * 0.5f);
    ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)(intptr_t)tex, q, ImVec2(q.x + iw, q.y + ih), ImVec2(0, 0),
                                                ImVec2(1, 1), IM_COL32_WHITE, 2.f * s);
    ImGui::Dummy(ImVec2(w, ih));
  }
  ImGui::Dummy(ImVec2(0, 4 * s));
  uiCardBegin("##cdetail", nullptr, false);
  ImGui::PushFont(g_fontBold);
  ImGui::TextWrapped("%s", fileName(p.path).c_str());
  ImGui::PopFont();
  char dims[48];
  snprintf(dims, sizeof(dims), "%d × %d", p.w, p.h);
  uiHint(dims);
  ImGui::PushFont(g_fontBold);
  ImGui::Text("%s  %.1f", tr(T_Score), e.score);
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal::kAccent), "#%d", selected_ + 1);
  w = ImGui::GetContentRegionAvail().x;
  auto bar = [&](int key, float v) {
    char b[16];
    snprintf(b, sizeof(b), "%.0f", v * 100);
    ImGui::TextUnformatted(tr(key));
    ImGui::SameLine(w * 0.42f);
    ImGui::ProgressBar(v, ImVec2(-FLT_MIN, 14 * s), b);
  };
  bar(T_Technical, e.tech);
  bar(T_Aesthetic, e.aes);
  bar(T_Composition, e.comp);
  if (rs_.refMode && refs_.size() >= 5) bar(T_Similarity, e.sim);
  ImGui::Spacing();
  for (int k = 0; k < (int)ai::R_COUNT_BITS; k++)
    if (e.reasons & (1u << k)) {
      bool good = (1u << k) & kGoodReasons;
      ImGui::TextColored(good ? ImVec4(0.55f, 0.86f, 0.55f, 1) : ImVec4(1.f, 0.6f, 0.47f, 1), "%s %s", good ? "+" : "-",
                         tr(kReasonKeys[k]));
    }
  if (p.faces > 0) ImGui::TextDisabled("%s: %d", tr(T_Faces), p.faces);
  if (e.groupSize > 1) ImGui::TextDisabled("%s %d %s", tr(T_BurstOf), e.groupSize, tr(T_PhotosUnit));
  uiCardEnd();
  if (uiPrimaryButton(trId(T_OpenInEditor, "openedit"))) h.openInEditor(p.path);
  if (p.hasAi && ImGui::Button(trId(T_SimilarTo, "similar"), ImVec2(-FLT_MIN, 0))) {
    std::memcpy(qEmb_, p.emb, sizeof(qEmb_));
    searchLabel_ = fileName(p.path);
    searchEnglish_.clear();
    searchActive_ = true;
    selected_ = 0;
    rankDirty_ = true;
  }
}

void CullView::drawSearchBar(CullHost& h) {
  float s = h.scale;
  ImGui::SetNextItemWidth(std::max(160.f * s, ImGui::GetContentRegionAvail().x - 330 * s));
  bool enter = ImGui::InputTextWithHint("##q", tr(T_SearchHint), query_, sizeof(query_), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  ImGui::BeginDisabled(searchRunning_ || photos_.empty());
  if ((uiPrimaryButton(trId(T_Search, "dosearch"), ImVec2(0, 0)) || enter) && query_[0]) startSearch(query_);
  ImGui::EndDisabled();
  if (searchActive_) {
    ImGui::SameLine();
    if (ImGui::Button("×")) {
      searchActive_ = false;
      query_[0] = 0;
      rankDirty_ = true;
    }
  }
  ImGui::SameLine(0, 16 * s);
  {
    float before = strict_;
    ImGui::BeginChild("##strict", ImVec2(ImGui::GetContentRegionAvail().x - 14 * s, 0), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    EditResult r;
    uiSlider(r, T_Strictness, "strict", &strict_, 0, 100, 40, "%.0f");
    ImGui::EndChild();
    if (strict_ != before && searchActive_) applySearch();
  }
  if (searchRunning_) {
    ImGui::TextDisabled("%s", tr(T_Searching));
  } else if (searchActive_) {
    if (!searchEnglish_.empty() && searchEnglish_ != searchLabel_)
      ImGui::TextDisabled("%s \"%s\" (%s)  -  %d %s", tr(T_SearchFor), searchLabel_.c_str(), searchEnglish_.c_str(),
                          searchMatches_, tr(T_Matches));
    else
      ImGui::TextDisabled("%s \"%s\"  -  %d %s", tr(T_SearchFor), searchLabel_.c_str(), searchMatches_, tr(T_Matches));
  }
}

void CullView::draw(CullHost& h, StyleLibrary& lib) {
  float s = h.scale;
  float leftW = 330 * s, rightW = 310 * s;
  float bodyH = ImGui::GetContentRegionAvail().y;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 12 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 6 * s));
  ImGui::BeginChild("##cullleft", ImVec2(leftW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  tourSetWindow(TR_CLEFT);
  drawControls(h, lib);
  ImGui::EndChild();
  ImGui::SameLine(0, 0);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(pal::kCanvas));
  ImGui::BeginChild("##cullmid", ImVec2(ImGui::GetContentRegionAvail().x - rightW, bodyH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar);
  tourSetWindow(TR_CGRID);
  ImGui::SetCursorPos(ImVec2(14 * s, 12 * s));
  ImGui::BeginGroup();
  drawSearchBar(h);
  ImGui::EndGroup();
  ImGui::BeginChild("##cullgrid", ImVec2(0, 0), ImGuiChildFlags_None);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  drawGrid(h);
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 0);
  ImGui::BeginChild("##cullright", ImVec2(rightW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  drawDetail(h);
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  // Keyboard: arrows move the selection, Enter opens it
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput && !ranked_.empty()) {
    int n = shownCount();
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) selected_ = std::min(n - 1, selected_ + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) selected_ = std::max(0, selected_ - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && selected_ >= 0) h.openInEditor(photos_[ranked_[selected_].idx].path);
  }
}
