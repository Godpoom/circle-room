#include "ai/cache.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "platform_win.h"

namespace ai {

static const char kMagic[4] = {'P', 'E', 'A', 'I'};
static const uint32_t kVersion = 2;  // v2: adaptive neutral zones in ToneSig

// Limits for files read back from disk. A folder can hold someone else's cache files, so a
// damaged or crafted one must never make us allocate gigabytes or read past the data.
static const uint64_t kMaxCacheBytes = 512ull << 20;  // ~200k photos at ~2.5 KB each
static const uint32_t kMaxEntries = 200000;
static const uint32_t kMaxThumbBytes = 8u << 20;       // thumbnails are ~20-60 KB JPEGs

static std::string cachePath(const std::string& folder) { return folder + "\\.photoeditor_ai"; }
static std::string thumbPath(const std::string& folder) { return folder + "\\.photoeditor_thumbs"; }

namespace {
struct Writer {
  std::vector<uint8_t> b;
  void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
  template <class T>
  void pod(const T& v) { raw(&v, sizeof(T)); }
  void str(const std::string& s) {
    pod((uint32_t)s.size());
    raw(s.data(), s.size());
  }
};
struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  void raw(void* d, size_t n) {
    if ((size_t)(end - p) < n) { ok = false; std::memset(d, 0, n); return; }
    std::memcpy(d, p, n);
    p += n;
  }
  template <class T>
  void pod(T& v) { raw(&v, sizeof(T)); }
  void str(std::string& s) {
    uint32_t n = 0;
    pod(n);
    if (!ok || n > 4096 || (size_t)(end - p) < n) { ok = false; return; }
    s.assign((const char*)p, n);
    p += n;
  }
};
}  // namespace

bool saveCache(const std::string& folder, const std::vector<PhotoInfo>& items) {
  Writer w;
  w.raw(kMagic, 4);
  w.pod(kVersion);
  w.pod((uint32_t)sizeof(Features));
  w.pod((uint32_t)items.size());
  for (const PhotoInfo& p : items) {
    w.str(fileName(p.path));
    w.pod(p.size); w.pod(p.mtime); w.pod(p.w); w.pod(p.h); w.pod(p.shot);
    w.pod(p.feat);
    w.pod(p.faces); w.pod(p.faceSharp); w.pod(p.faceArea); w.pod(p.eyesOpen);
    uint8_t hasAi = p.hasAi;
    w.pod(hasAi);
    w.pod(p.aesthetic);
    w.raw(p.prompts, sizeof(p.prompts));
    w.raw(p.emb, sizeof(p.emb));
    w.raw(&p.tone, sizeof(p.tone));
    w.pod(p.thumbOff); w.pod(p.thumbLen);
  }
  std::string tmp = cachePath(folder) + ".tmp";
  FILE* f = ufopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(w.b.data(), 1, w.b.size(), f) == w.b.size();
  ok = (fclose(f) == 0) && ok;
  if (!ok) { deleteFileU(tmp); return false; }
  return replaceFileU(tmp, cachePath(folder)) && setHiddenU(cachePath(folder));
}

// Smallest possible record: empty name plus every fixed-size field.
static size_t minRecordBytes() {
  PhotoInfo p;
  return sizeof(uint32_t) + sizeof(p.size) + sizeof(p.mtime) + sizeof(p.w) + sizeof(p.h) + sizeof(p.shot) +
         sizeof(p.feat) + sizeof(p.faces) + sizeof(p.faceSharp) + sizeof(p.faceArea) + sizeof(p.eyesOpen) + 1 +
         sizeof(p.aesthetic) + sizeof(p.prompts) + sizeof(p.emb) + sizeof(p.tone) + sizeof(p.thumbOff) + sizeof(p.thumbLen);
}

static bool finite(const float* v, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (!std::isfinite(v[i])) return false;
  return true;
}

// Values that later feed sorting and maths: NaN would break std::sort's ordering.
static bool plausible(const PhotoInfo& p) {
  static_assert(sizeof(Features) % sizeof(float) == 0, "Features holds floats only");
  return p.w >= 0 && p.h >= 0 && p.faces >= 0 && p.faces <= 1000 &&
         finite((const float*)&p.feat, sizeof(p.feat) / sizeof(float)) && std::isfinite(p.faceSharp) &&
         std::isfinite(p.faceArea) && std::isfinite(p.eyesOpen) && std::isfinite(p.aesthetic) &&
         finite(p.prompts, P_COUNT) && finite(p.emb, kEmbDim) && finite(p.tone.v, kToneDim) && p.thumbLen <= kMaxThumbBytes;
}

bool loadCache(const std::string& folder, std::vector<PhotoInfo>& out) {
  out.clear();
  unsigned long long fsize = 0;
  long long mtime = 0;
  if (!fileInfo(cachePath(folder), fsize, mtime) || fsize > kMaxCacheBytes) return false;  // too big: re-analyse
  std::vector<unsigned char> b;
  if (!readFileBytes(cachePath(folder), b)) return false;
  Reader r{b.data(), b.data() + b.size()};
  char magic[4];
  uint32_t ver = 0, featSize = 0, n = 0;
  r.raw(magic, 4);
  r.pod(ver);
  r.pod(featSize);
  r.pod(n);
  if (!r.ok || std::memcmp(magic, kMagic, 4) != 0 || ver != kVersion || featSize != sizeof(Features)) return false;
  // The count must fit in the bytes that follow; otherwise the file is damaged.
  if (n > kMaxEntries || (size_t)(r.end - r.p) / minRecordBytes() < n) return false;
  out.reserve(n);
  for (uint32_t i = 0; i < n && r.ok; i++) {
    PhotoInfo p;
    std::string name;
    r.str(name);
    p.path = folder + "\\" + name;
    r.pod(p.size); r.pod(p.mtime); r.pod(p.w); r.pod(p.h); r.pod(p.shot);
    r.pod(p.feat);
    r.pod(p.faces); r.pod(p.faceSharp); r.pod(p.faceArea); r.pod(p.eyesOpen);
    uint8_t hasAi = 0;
    r.pod(hasAi);
    p.hasAi = hasAi != 0;
    r.pod(p.aesthetic);
    r.raw(p.prompts, sizeof(p.prompts));
    r.raw(p.emb, sizeof(p.emb));
    r.raw(&p.tone, sizeof(p.tone));
    r.pod(p.thumbOff); r.pod(p.thumbLen);
    if (r.ok && plausible(p)) out.push_back(std::move(p));  // a bad entry is just analysed again
  }
  if (!r.ok) out.clear();
  return r.ok;
}

int clearFolderCache(const std::string& folder) {
  int n = 0;
  for (const std::string& f : {cachePath(folder), cachePath(folder) + ".tmp", thumbPath(folder)})
    n += deleteFileU(f) ? 1 : 0;
  std::string dir = folder + "\\.photoeditor_cache";
  for (const std::string& f : listFiles(dir, "")) {
    std::string name = fileName(f);
    auto ends = [&](const char* suf) {
      size_t k = std::strlen(suf);
      return name.size() >= k && name.compare(name.size() - k, k, suf) == 0;
    };
    bool ours = ends(".recover.png") || (ends(".png") && name.find(".heal-") != std::string::npos) ||
                ends(".png.part") || ends(".tmp");
    if (ours && deleteFileU(f)) n++;
  }
  return n;
}

bool ThumbStore::openAppend(const std::string& folder) {
  close();
  f_ = ufopen(thumbPath(folder), "ab");
  if (f_) setHiddenU(thumbPath(folder));
  return f_ != nullptr;
}

bool ThumbStore::append(const std::vector<uint8_t>& jpeg, uint64_t& off, uint32_t& len) {
  if (!f_) return false;
  _fseeki64(f_, 0, SEEK_END);
  off = (uint64_t)_ftelli64(f_);
  len = (uint32_t)jpeg.size();
  return fwrite(jpeg.data(), 1, jpeg.size(), f_) == jpeg.size();
}

void ThumbStore::close() {
  if (f_) fclose(f_);
  f_ = nullptr;
}

bool readThumb(const std::string& folder, uint64_t off, uint32_t len, std::vector<uint8_t>& out) {
  out.clear();
  if (!len || len > kMaxThumbBytes) return false;
  FILE* f = ufopen(thumbPath(folder), "rb");
  if (!f) return false;
  // The slice must lie inside the file before anything is allocated.
  bool ok = _fseeki64(f, 0, SEEK_END) == 0;
  long long size = ok ? _ftelli64(f) : -1;
  ok = ok && size >= 0 && off <= (uint64_t)size && len <= (uint64_t)size - off;
  if (ok) {
    out.resize(len);
    ok = _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(out.data(), 1, len, f) == len;
  }
  fclose(f);
  if (!ok) out.clear();
  return ok;
}

}  // namespace ai
