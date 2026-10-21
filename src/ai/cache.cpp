#include "ai/cache.h"

#include <cstdio>
#include <cstring>

#include "platform_win.h"

namespace ai {

static const char kMagic[4] = {'P', 'E', 'A', 'I'};
static const uint32_t kVersion = 2;  // v2: adaptive neutral zones in ToneSig

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

bool loadCache(const std::string& folder, std::vector<PhotoInfo>& out) {
  out.clear();
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
    if (r.ok) out.push_back(std::move(p));
  }
  return r.ok;
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
  if (!len) return false;
  FILE* f = ufopen(thumbPath(folder), "rb");
  if (!f) return false;
  out.resize(len);
  bool ok = _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(out.data(), 1, len, f) == len;
  fclose(f);
  return ok;
}

}  // namespace ai
