#include "ai/cull.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>

#include "image_io.h"
#include "platform_win.h"

namespace ai {

static const int kAnalysisSize = 768;
static const int kThumbSize = 256;
// Laplacian-energy (log10) range mapped to "blurry .. sharp"; calibrated on test sets.
static const float kSharpLo = -4.4f, kSharpHi = -2.8f;
static const float kNoiseLo = 0.006f, kNoiseHi = 0.03f;

static float smooth01(float a, float b, float x) {
  float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3 - 2 * t);
}

// ---------------------------------------------------------------- per-image stages
namespace {
struct Prepared {
  int idx = 0;
  bool ok = false;
  SmallImage img;
  DecodeInfo info;
  PhotoInfo out;
  std::vector<uint8_t> thumb;
  std::vector<float> tensor;
};
}  // namespace

static void cpuStage(Prepared& p) {
  computeFeatures(p.img, p.out.feat);
  SmallImage t;
  fitLongEdge(p.img, kThumbSize, t);
  toneSignature(t.rgb.data(), t.w, t.h, 3, p.out.tone);
  encodeJpegMem(t.rgb.data(), t.w, t.h, 82, p.thumb);
  p.tensor.resize(kClipTensor);
  Clip::prep(p.img, p.tensor.data());
}

static void modelStage(std::vector<Prepared*>& batch, AnalyzeModels& m) {
  std::vector<float> tensors;
  std::vector<std::pair<int, int>> owner;  // (batch item, -1 = whole image | face index)
  for (int i = 0; i < (int)batch.size(); i++) {
    Prepared& p = *batch[i];
    if (!p.ok) continue;
    tensors.insert(tensors.end(), p.tensor.begin(), p.tensor.end());
    owner.push_back({i, -1});
    if (!m.face.ok()) continue;
    std::vector<Face> faces;
    std::string err;
    if (!m.face.detect(p.img, faces, err)) continue;
    float area = (float)p.img.w * p.img.h;
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.w * a.h > b.w * b.h; });
    int kept = 0;
    for (const Face& f : faces) {
      if (f.w * f.h < area * 0.004f || kept >= 3) continue;
      kept++;
      p.out.faceArea = std::max(p.out.faceArea, f.w * f.h / area);
      p.out.faceSharp = std::max(p.out.faceSharp, regionSharpness(p.img, (int)f.x, (int)f.y, (int)(f.x + f.w), (int)(f.y + f.h)));
      float side = 1.5f * std::max(f.w, f.h);
      std::vector<float> t(kClipTensor);
      Clip::prep(p.img, t.data(), f.x + f.w / 2 - side / 2, f.y + f.h / 2 - side / 2, side);
      tensors.insert(tensors.end(), t.begin(), t.end());
      owner.push_back({i, kept});
    }
    p.out.faces = kept;
  }
  if (!m.clip.ok() || owner.empty()) return;
  std::vector<float> emb(owner.size() * kEmbDim);
  std::string err;
  for (size_t s = 0; s < owner.size(); s += 16) {
    int n = (int)std::min<size_t>(16, owner.size() - s);
    if (!m.clip.embed(&tensors[s * kClipTensor], n, &emb[s * kEmbDim], err)) return;
  }
  for (size_t k = 0; k < owner.size(); k++) {
    PhotoInfo& o = batch[owner[k].first]->out;
    const float* e = &emb[k * kEmbDim];
    if (owner[k].second < 0) {
      std::copy(e, e + kEmbDim, o.emb);
      o.aesthetic = m.clip.aesthetic(e);
      for (int q = 0; q < P_COUNT; q++) o.prompts[q] = m.clip.prompt(e, q);
      o.hasAi = true;
    } else {
      o.eyesOpen = std::min(o.eyesOpen, m.clip.prompt(e, P_EYES));
    }
  }
}

void analyzeImage(const SmallImage& img, AnalyzeModels* models, PhotoInfo& out, std::vector<uint8_t>* thumbJpeg) {
  Prepared p;
  fitLongEdge(img, kAnalysisSize, p.img);
  p.ok = true;
  cpuStage(p);
  if (models) {
    std::vector<Prepared*> b{&p};
    modelStage(b, *models);
  }
  if (thumbJpeg) thumbJpeg->swap(p.thumb);
  out = std::move(p.out);
}

// ---------------------------------------------------------------- Analyzer
Analyzer::~Analyzer() {
  cancel_ = true;
  wait();
}

void Analyzer::wait() {
  if (th_.joinable()) th_.join();
}

void Analyzer::startFolder(const std::string& folder, bool preferGpu) {
  if (running_) return;
  wait();
  folder_ = folder;
  files_ = listImages(folder);
  cancel_ = false;
  finished_ = false;
  running_ = true;
  done_ = 0;
  total_ = 0;
  th_ = std::thread([this, preferGpu] { runGuarded(preferGpu); });
}

void Analyzer::startFiles(const std::vector<std::string>& files, bool preferGpu) {
  if (running_) return;
  wait();
  folder_.clear();
  files_ = files;
  cancel_ = false;
  finished_ = false;
  running_ = true;
  done_ = 0;
  total_ = 0;
  th_ = std::thread([this, preferGpu] { runGuarded(preferGpu); });
}

// Out of memory on some photo: end the job with what was analysed instead of ending the app.
void Analyzer::runGuarded(bool preferGpu) {
  try {
    run(preferGpu);
  } catch (const std::exception& e) {
    {
      std::lock_guard<std::mutex> lk(msgMutex_);
      msg_ = std::string("analysis stopped: ") + e.what();
    }
    running_ = false;
    finished_ = true;
  }
}

float Analyzer::progress() const { return total_ > 0 ? (float)done_ / total_ : 0.f; }

bool Analyzer::poll() {
  if (!finished_) return false;
  finished_ = false;
  wait();
  return true;
}

std::vector<PhotoInfo> Analyzer::takeResults() { return std::move(results_); }

std::string Analyzer::message() const {
  std::lock_guard<std::mutex> lk(msgMutex_);
  return msg_;
}

void Analyzer::run(bool preferGpu) {
  results_.clear();
  bool folderMode = !folder_.empty();
  // Reuse cached entries whose file is unchanged.
  std::map<std::string, PhotoInfo> cached;
  if (folderMode) {
    std::vector<PhotoInfo> old;
    loadCache(folder_, old);
    for (auto& p : old) cached[fileName(p.path)] = std::move(p);
  }
  std::vector<std::string> todo;
  std::vector<std::pair<unsigned long long, long long>> todoInfo;
  for (const std::string& f : files_) {
    unsigned long long size = 0;
    long long mtime = 0;
    if (!fileInfo(f, size, mtime)) continue;
    auto it = cached.find(fileName(f));
    if (it != cached.end() && it->second.size == size && it->second.mtime == mtime && it->second.hasAi) {
      it->second.path = f;
      results_.push_back(std::move(it->second));
      continue;
    }
    todo.push_back(f);
    todoInfo.push_back({size, mtime});
  }
  total_ = (int)todo.size();

  AnalyzeModels models;
  if (!todo.empty()) {
    std::string err;
    bool aiOk = models.clip.load(preferGpu, err);
    std::string ferr;
    if (aiOk) models.face.load(ferr);
    std::lock_guard<std::mutex> lk(msgMutex_);
    msg_ = aiOk ? (models.clip.onGpu() ? "GPU (DirectML)" : "CPU") : "AI unavailable: " + err;
  }
  ThumbStore thumbs;
  if (folderMode && !todo.empty()) thumbs.openAppend(folder_);

  std::mutex qm;
  std::condition_variable cvPush, cvPop;
  std::deque<Prepared*> queue;
  std::atomic<int> next{0}, producersLeft{0};
  const size_t cap = 8;
  int workers = (int)std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 4u);
  producersLeft = workers;
  std::vector<std::thread> pool;
  for (int w = 0; w < workers && !todo.empty(); w++) {
    pool.emplace_back([&] {
      for (;;) {
        int i = next++;
        if (i >= (int)todo.size() || cancel_) break;
        auto* p = new Prepared();
        p->idx = i;
        std::string err;
        p->ok = decodeScaled(todo[i], kAnalysisSize, p->img, &p->info, err);
        if (p->ok) cpuStage(*p);
        std::unique_lock<std::mutex> lk(qm);
        cvPush.wait(lk, [&] { return queue.size() < cap || cancel_; });
        queue.push_back(p);
        cvPop.notify_one();
      }
      std::lock_guard<std::mutex> lk(qm);
      producersLeft--;
      cvPop.notify_all();
    });
  }
  if (todo.empty()) producersLeft = 0;

  int sinceSave = 0;
  for (;;) {
    std::vector<Prepared*> batch;
    {
      std::unique_lock<std::mutex> lk(qm);
      cvPop.wait(lk, [&] { return !queue.empty() || producersLeft == 0; });
      if (queue.empty() && producersLeft == 0) break;
      while (!queue.empty() && batch.size() < 8) {
        batch.push_back(queue.front());
        queue.pop_front();
      }
      cvPush.notify_all();
    }
    modelStage(batch, models);
    for (Prepared* p : batch) {
      if (p->ok) {
        PhotoInfo& o = p->out;
        o.path = todo[p->idx];
        o.size = todoInfo[p->idx].first;
        o.mtime = todoInfo[p->idx].second;
        o.w = p->info.origW;
        o.h = p->info.origH;
        o.shot = p->info.shotTime;
        if (folderMode) thumbs.append(p->thumb, o.thumbOff, o.thumbLen);
        results_.push_back(std::move(o));
      }
      delete p;
      done_++;
    }
    if (folderMode && (sinceSave += (int)batch.size()) >= 200) {  // resumable if interrupted
      saveCache(folder_, results_);
      sinceSave = 0;
    }
  }
  for (auto& t : pool) t.join();
  thumbs.close();
  if (folderMode && total_ > 0) saveCache(folder_, results_);
  models.clip.release();
  models.face.release();
  ortUnload();
  // Keep the folder order stable for the UI.
  std::sort(results_.begin(), results_.end(), [](const PhotoInfo& a, const PhotoInfo& b) { return a.path < b.path; });
  running_ = false;
  finished_ = true;
}

// ---------------------------------------------------------------- ranking
static float dot(const float* a, const float* b) {
  float s = 0;
  for (int k = 0; k < kEmbDim; k++) s += a[k] * b[k];
  return s;
}

std::vector<int> groupDuplicates(const std::vector<PhotoInfo>& photos) {
  int n = (int)photos.size();
  std::vector<int> parent(n);
  for (int i = 0; i < n; i++) parent[i] = i;
  std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
  std::vector<int> order(n);
  for (int i = 0; i < n; i++) order[i] = i;
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (photos[a].shot != photos[b].shot) return photos[a].shot < photos[b].shot;
    return photos[a].path < photos[b].path;
  });
  for (int a = 0; a < n; a++) {
    const PhotoInfo& p = photos[order[a]];
    for (int b = a + 1; b < std::min(n, a + 40); b++) {
      const PhotoInfo& q = photos[order[b]];
      bool timed = p.shot && q.shot;
      if (timed && q.shot - p.shot > 180) break;
      bool same;
      if (p.hasAi && q.hasAi) same = dot(p.emb, q.emb) > (timed ? 0.93f : 0.97f);
      else same = timed && toneDistance(p.tone, q.tone) < 0.04f;
      if (same) parent[find(order[a])] = find(order[b]);
    }
  }
  std::vector<int> g(n);
  for (int i = 0; i < n; i++) g[i] = find(i);
  return g;
}

static std::vector<float> percentiles(const std::vector<float>& v) {
  std::vector<int> idx(v.size());
  for (size_t i = 0; i < v.size(); i++) idx[i] = (int)i;
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return v[a] < v[b]; });
  std::vector<float> p(v.size(), 0.5f);
  for (size_t r = 0; r < idx.size(); r++) p[idx[r]] = idx.size() > 1 ? (float)r / (idx.size() - 1) : 0.5f;
  return p;
}

// Balanced logistic regression: references (positive) vs the folder (unlabelled negatives).
static std::vector<float> personalScores(const std::vector<PhotoInfo>& photos, const std::vector<PhotoInfo>& refs) {
  std::vector<float> w(kEmbDim, 0.f), out(photos.size(), 0.5f);
  float b = 0;
  std::vector<const PhotoInfo*> pos, neg;
  for (auto& r : refs)
    if (r.hasAi) pos.push_back(&r);
  for (auto& p : photos)
    if (p.hasAi) neg.push_back(&p);
  if (pos.empty() || neg.empty()) return out;
  const float lr = 4.f, l2 = 1e-3f;
  std::vector<float> grad(kEmbDim);
  for (int it = 0; it < 150; it++) {
    std::fill(grad.begin(), grad.end(), 0.f);
    float gb = 0;
    auto acc = [&](const PhotoInfo* p, float y, float weight) {
      float z = dot(w.data(), p->emb) + b;
      float d = (1.f / (1.f + std::exp(-z)) - y) * weight;
      for (int k = 0; k < kEmbDim; k++) grad[k] += d * p->emb[k];
      gb += d;
    };
    for (auto* p : pos) acc(p, 1.f, 0.5f / pos.size());
    for (auto* p : neg) acc(p, 0.f, 0.5f / neg.size());
    for (int k = 0; k < kEmbDim; k++) w[k] -= lr * (grad[k] + l2 * w[k]);
    b -= lr * gb;
  }
  for (size_t i = 0; i < photos.size(); i++)
    if (photos[i].hasAi) out[i] = dot(w.data(), photos[i].emb) + b;
  return percentiles(out);
}

std::vector<RankEntry> rankPhotos(const std::vector<PhotoInfo>& photos, const std::vector<int>& groups,
                                  const std::vector<PhotoInfo>& refs, const RankSettings& s) {
  size_t n = photos.size();
  std::vector<RankEntry> all(n);
  std::vector<float> sharpRaw(n);
  for (size_t i = 0; i < n; i++) {
    const PhotoInfo& p = photos[i];
    sharpRaw[i] = p.faces > 0 && p.faceArea > 0.01f ? 0.4f * p.feat.sharp + 0.6f * p.faceSharp : p.feat.sharp;
  }
  std::vector<float> sharpPct = percentiles(sharpRaw);
  bool useRefs = s.refMode && !refs.empty();
  std::vector<float> personal;
  if (useRefs) personal = personalScores(photos, refs);
  float wsum = std::max(1e-3f, s.wTech + s.wAes + s.wComp);
  for (size_t i = 0; i < n; i++) {
    const PhotoInfo& p = photos[i];
    const Features& f = p.feat;
    RankEntry& e = all[i];
    e.idx = (int)i;
    float sharp = 0.6f * smooth01(kSharpLo, kSharpHi, sharpRaw[i]) + 0.4f * sharpPct[i];
    if (p.hasAi) sharp = 0.8f * sharp + 0.2f * p.prompts[P_SHARPNESS];
    float expo = 1.f - 0.6f * smooth01(0.01f, 0.12f, f.clipHi) - 0.4f * smooth01(0.03f, 0.25f, f.clipLo) -
                 0.3f * smooth01(0.25f, 0.45f, std::fabs(f.meanL - 0.47f));
    expo = std::clamp(expo, 0.f, 1.f);
    if (p.hasAi) expo = 0.7f * expo + 0.3f * p.prompts[P_EXPOSURE];
    float noise = 1.f - smooth01(kNoiseLo, kNoiseHi, f.noise);
    e.tech = 0.6f * sharp + 0.25f * expo + 0.15f * noise;
    if (p.hasAi)
      e.aes = 0.6f * smooth01(3.5f, 6.5f, p.aesthetic) + 0.2f * p.prompts[P_QUALITY] + 0.2f * p.prompts[P_LIGHTING];
    else
      e.aes = 0.5f * f.dynRange + 0.5f * expo;
    e.comp = p.hasAi ? 0.35f * p.prompts[P_COMPOSITION] + 0.15f * p.prompts[P_SUBJECT] + 0.1f * p.prompts[P_LEVEL] +
                           0.4f * f.compHeur
                     : f.compHeur;
    float quality = (s.wTech * e.tech + s.wAes * e.aes + s.wComp * e.comp) / wsum;
    bool eyesClosed = p.faces > 0 && p.eyesOpen < 0.3f;
    // Technical faults hurt more than CLIP's aesthetic score notices: scale the whole score.
    quality *= 0.55f + 0.45f * smooth01(0.1f, 0.6f, sharp);
    quality *= 1.f - 0.45f * smooth01(0.06f, 0.3f, f.clipHi) - 0.3f * smooth01(0.2f, 0.5f, f.clipLo);
    if (eyesClosed) quality *= 0.75f;
    float fin = quality;
    if (useRefs) {
      std::vector<float> cs, td;
      for (const PhotoInfo& r : refs) {
        if (p.hasAi && r.hasAi) cs.push_back(dot(p.emb, r.emb));
        td.push_back(toneDistance(p.tone, r.tone));
      }
      std::sort(cs.rbegin(), cs.rend());
      std::sort(td.begin(), td.end());
      float content = 0;
      if (!cs.empty()) {
        size_t k = std::min<size_t>(3, cs.size());
        for (size_t q = 0; q < k; q++) content += cs[q];
        content = smooth01(0.55f, 0.9f, content / k);
      }
      float tone = std::exp(-(td.size() > 1 ? 0.5f * (td[0] + td[1]) : td[0]) / 0.12f);
      e.sim = p.hasAi ? 0.4f * content + 0.3f * tone + 0.3f * personal[i] : tone;
      fin = s.alpha / 100.f * quality + (1.f - s.alpha / 100.f) * e.sim;
    }
    e.score = std::round(100.f * std::clamp(fin, 0.f, 1.f) * 10.f) / 10.f;
    uint32_t r = 0;
    if (sharp < 0.3f) r |= R_BLURRY;
    if (p.faces > 0 && smooth01(kSharpLo, kSharpHi, p.faceSharp) < 0.3f) r |= R_FACE_BLUR;
    if (eyesClosed) r |= R_EYES_CLOSED;
    if (f.clipHi > 0.08f) r |= R_OVEREXPOSED;
    if (f.clipLo > 0.35f || (f.meanL < 0.1f && f.dynRange < 0.5f)) r |= R_UNDEREXPOSED;  // low-key is fine
    if (noise < 0.4f) r |= R_NOISY;
    if (f.tiltConf > 0.5f && std::fabs(f.tilt) > 1.f) r |= R_TILTED;
    if (f.edgeCut > 0.3f) r |= R_EDGE_CUT;
    if (f.balance > 0.7f) r |= R_OFF_BALANCE;
    if (e.comp > 0.7f) r |= R_GOOD_COMP;
    if (sharp > 0.8f) r |= R_SHARP;
    if (p.hasAi && p.prompts[P_LIGHTING] > 0.6f) r |= R_GOOD_LIGHT;
    if (useRefs && e.sim > 0.7f) r |= R_LIKE_REF;
    e.reasons = r;
  }
  // Groups: best member represents the group.
  std::map<int, int> best, size;
  for (size_t i = 0; i < n; i++) {
    int g = groups.empty() ? (int)i : groups[i];
    all[i].group = g;
    size[g]++;
    auto it = best.find(g);
    if (it == best.end() || all[i].score > all[it->second].score) best[g] = (int)i;
  }
  std::vector<RankEntry> out;
  for (size_t i = 0; i < n; i++) {
    RankEntry& e = all[i];
    e.groupSize = size[e.group];
    e.groupBest = best[e.group] == (int)i;
    if (!e.groupBest) e.reasons |= R_DUPLICATE;
    if (!s.dedupe || e.groupBest) out.push_back(e);
  }
  std::stable_sort(out.begin(), out.end(), [](const RankEntry& a, const RankEntry& b) { return a.score > b.score; });
  return out;
}

}  // namespace ai
