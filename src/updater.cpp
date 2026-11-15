#include "updater.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "mcp_json.h"
#include "platform_win.h"
#include "stb_image.h"  // zlib inflate (the implementation lives in image_io.cpp)
#include "version.h"

namespace {

const char* kFeed = "https://api.github.com/repos/Godpoom/circle-room/releases/latest";
const char* kSums = "SHA256SUMS.txt";

// The request in flight, so shutdown() can abort a stalled download at once.
std::mutex g_reqM;
HINTERNET g_req = nullptr;

bool localHost(const std::wstring& h) { return h == L"127.0.0.1" || h == L"localhost"; }

// GET into memory (body) or into a file. Plain http only for a local test server.
bool httpGet(const std::string& url, std::string* body, const std::string& file, size_t maxBody,
             std::atomic<long long>* got, std::atomic<long long>* total, std::atomic<bool>& cancel, std::string& err) {
  std::wstring w = utf8ToWide(url);
  wchar_t host[256], path[2048], extra[2048];
  URL_COMPONENTS uc;
  std::memset(&uc, 0, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) {
    err = "!network bad address";
    return false;
  }
  bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  std::wstring h(host, uc.dwHostNameLength);
  if (!https && !localHost(h)) {
    err = "!network refusing plain http";
    return false;
  }
  std::wstring agent = utf8ToWide(std::string("CircleRoom-Updater/") + CR_VERSION_STR);
  HINTERNET ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    err = "!network " + std::to_string(GetLastError());
    return false;
  }
  WinHttpSetTimeouts(ses, 10000, 10000, 15000, 20000);
  HINTERNET con = WinHttpConnect(ses, h.c_str(), uc.nPort, 0);
  std::wstring obj = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
  HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
  {
    std::lock_guard<std::mutex> lk(g_reqM);
    g_req = req;
  }
  bool ok = false;
  FILE* out = nullptr;
  if (req && !cancel) {
    const wchar_t* hdr = L"Accept: application/vnd.github+json, application/octet-stream, */*\r\n";
    if (WinHttpSendRequest(req, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
      DWORD status = 0, sz = sizeof(status);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                          &sz, WINHTTP_NO_HEADER_INDEX);
      wchar_t len[32];
      DWORD lsz = sizeof(len);
      if (total && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsz,
                                       WINHTTP_NO_HEADER_INDEX))
        *total = _wtoi64(len);
      if (status != 200) {
        err = "!http " + std::to_string(status);
      } else {
        if (!file.empty()) out = ufopen(file, "wb");
        if (!file.empty() && !out) {
          err = "!nowrite";
        } else {
          std::vector<char> buf(1 << 16);
          ok = true;
          for (;;) {
            if (cancel) {
              err = "!cancelled";
              ok = false;
              break;
            }
            DWORD rd = 0;
            if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &rd)) {
              err = cancel ? "!cancelled" : "!network " + std::to_string(GetLastError());
              ok = false;
              break;
            }
            if (rd == 0) break;
            if (out && fwrite(buf.data(), 1, rd, out) != rd) {
              err = "!nowrite";
              ok = false;
              break;
            }
            if (body) {
              if (body->size() + rd > maxBody) {
                err = "!network response too large";
                ok = false;
                break;
              }
              body->append(buf.data(), rd);
            }
            if (got) *got += rd;
          }
        }
      }
    } else {
      err = cancel ? "!cancelled" : "!network " + std::to_string(GetLastError());
    }
  } else if (!req) {
    err = "!network " + std::to_string(GetLastError());
  }
  if (out && fclose(out) != 0 && ok) {
    ok = false;
    err = "!nowrite";
  }
  {
    std::lock_guard<std::mutex> lk(g_reqM);
    if (g_req == req && req) WinHttpCloseHandle(req);
    g_req = nullptr;
  }
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  return ok;
}

// "## Fixes\n**Bold** item" -> plain lines for the dialog.
std::string plainNotes(const std::string& md) {
  std::string out;
  size_t pos = 0;
  while (pos <= md.size() && out.size() < 6000) {
    size_t e = md.find('\n', pos);
    std::string ln = md.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    pos = e == std::string::npos ? md.size() + 1 : e + 1;
    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
    size_t i = 0;
    while (i < ln.size() && ln[i] == '#') i++;
    if (i) ln = ln.substr(ln.find_first_not_of(' ', i) == std::string::npos ? ln.size() : ln.find_first_not_of(' ', i));
    std::string clean;
    for (size_t k = 0; k < ln.size(); k++) {
      if (ln.compare(k, 2, "**") == 0) {
        k++;
        continue;
      }
      if (ln[k] == '`') continue;
      clean += ln[k];
    }
    if (clean.compare(0, 2, "- ") == 0 || clean.compare(0, 2, "* ") == 0) clean = "\xe2\x80\xa2 " + clean.substr(2);
    out += clean + "\n";
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

bool mkdirs(const std::string& dir) {
  if (dir.size() < 3 || isDirectory(dir)) return true;
  if (!mkdirs(dirOf(dir))) return false;
  return ensureDir(dir) || isDirectory(dir);
}

// Deletes a folder we own (the staging area), files first.
void removeTree(const std::string& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileW(utf8ToWide(dir + "\\*").c_str(), &fd);
  if (f != INVALID_HANDLE_VALUE) {
    do {
      std::wstring n = fd.cFileName;
      if (n == L"." || n == L"..") continue;
      std::string p = dir + "\\" + wideToUtf8(n);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // never follow links out
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
      else {
        SetFileAttributesW(utf8ToWide(p).c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(utf8ToWide(p).c_str());
      }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
  }
  SetFileAttributesW(utf8ToWide(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
  RemoveDirectoryW(utf8ToWide(dir).c_str());
}

bool crcOfFile(const std::string& path, uint32_t& crc) {
  FILE* f = ufopen(path, "rb");
  if (!f) return false;
  std::vector<char> buf(1 << 16);
  crc = 0;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) crc = crc32Of(buf.data(), n, crc);
  fclose(f);
  return true;
}

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

}  // namespace

// ---------------------------------------------------------------- small pieces
std::string updateFeedUrl() {
  wchar_t buf[1024];
  DWORD n = GetEnvironmentVariableW(L"CIRCLEROOM_UPDATE_FEED", buf, 1024);  // tests: a local fake release
  return n > 0 && n < 1024 ? wideToUtf8(buf) : std::string(kFeed);
}

int compareVersions(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  for (int part = 0; part < 4; part++) {
    long x = 0, y = 0;
    while (i < a.size() && a[i] >= '0' && a[i] <= '9') x = std::min(x * 10 + (a[i++] - '0'), 1000000L);
    while (j < b.size() && b[j] >= '0' && b[j] <= '9') y = std::min(y * 10 + (b[j++] - '0'), 1000000L);
    if (x != y) return x < y ? -1 : 1;
    if (i < a.size() && a[i] == '.') i++;
    if (j < b.size() && b[j] == '.') j++;
  }
  return 0;
}

bool parseRelease(const std::string& text, bool lite, ReleaseInfo& out, std::string& err) {
  Json j;
  if (!json::parse(text, j) || !j.isObj()) {
    err = "!badfeed";
    return false;
  }
  std::string tag = j.str("tag_name");
  if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
  if (tag.empty() || tag.size() > 20 || tag.find_first_not_of("0123456789.") != std::string::npos || !isdigit((unsigned char)tag[0])) {
    err = "!badfeed";
    return false;
  }
  out = ReleaseInfo();
  out.version = tag;
  out.notes = plainNotes(j.str("body"));
  out.pageUrl = j.str("html_url");
  out.assetName = lite ? "CircleRoom_Lite.zip" : "CircleRoom_Portable.zip";
  if (const Json* as = j.get("assets"))
    for (const Json& a : as->a) {
      if (!a.isObj()) continue;
      std::string n = a.str("name");
      if (n == out.assetName) {
        out.assetUrl = a.str("browser_download_url");
        out.assetSize = (long long)a.num("size", 0);
      } else if (n == kSums) {
        out.sumsUrl = a.str("browser_download_url");
      }
    }
  if (out.assetUrl.empty() || out.sumsUrl.empty()) {
    err = "!noasset";  // the update can still be downloaded by hand from the release page
    return true;
  }
  return true;
}

bool zipEntryPath(const std::string& name, std::string& rel) {
  const std::string root = "CircleRoom/";
  if (name.compare(0, root.size(), root) != 0 || name.size() == root.size() || name.back() == '/') return false;
  std::string r = name.substr(root.size());
  if (r.find('\\') != std::string::npos || r.find(':') != std::string::npos) return false;
  for (unsigned char c : r)
    if (c < 32) return false;
  size_t pos = 0;
  while (pos <= r.size()) {
    size_t e = r.find('/', pos);
    std::string part = r.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
    pos = e == std::string::npos ? r.size() + 1 : e + 1;
  }
  std::replace(r.begin(), r.end(), '/', '\\');
  std::string low = r;
  for (char& c : low) c = (char)tolower((unsigned char)c);
  // never the user's own files, nor our staging area
  if (low == "settings.ini" || low.compare(0, 8, "presets\\") == 0 || low.compare(0, 8, ".update\\") == 0 ||
      (low.size() > 4 && low.compare(low.size() - 4, 4, ".old") == 0))
    return false;
  rel = r;
  return true;
}

uint32_t crc32Of(const void* data, size_t n, uint32_t crc) {
  static uint32_t T[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      T[i] = c;
    }
    init = true;
  }
  const uint8_t* p = (const uint8_t*)data;
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = T[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

namespace {
struct Sha {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  bool ok = false;
  Sha() {
    ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
         BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
  }
  ~Sha() {
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  }
  void add(const void* d, size_t n) {
    const uint8_t* p = (const uint8_t*)d;
    while (ok && n > 0) {
      ULONG k = (ULONG)std::min<size_t>(n, 1u << 30);
      ok = BCryptHashData(h, (PUCHAR)p, k, 0) == 0;
      p += k;
      n -= k;
    }
  }
  std::string hex() {
    uint8_t d[32];
    if (!ok || BCryptFinishHash(h, d, 32, 0) != 0) return std::string();
    static const char* X = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
      s += X[b >> 4];
      s += X[b & 15];
    }
    return s;
  }
};
}  // namespace

std::string sha256Hex(const void* data, size_t n) {
  Sha s;
  s.add(data, n);
  return s.hex();
}

bool sha256File(const std::string& path, std::string& hex) {
  FILE* f = ufopen(path, "rb");
  if (!f) return false;
  Sha s;
  std::vector<char> buf(1 << 16);
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) s.add(buf.data(), n);
  fclose(f);
  hex = s.hex();
  return !hex.empty();
}

// ---------------------------------------------------------------- zip (stored / deflate, no zip64)
bool zipList(const std::string& zip, std::vector<ZipEntry>& out, std::string& err) {
  out.clear();
  err = "!badzip";
  FILE* f = ufopen(zip, "rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  long long size = _ftelli64(f);
  long long tailLen = std::min<long long>(size, 65535 + 22);
  std::vector<uint8_t> tail((size_t)tailLen);
  _fseeki64(f, size - tailLen, SEEK_SET);
  bool ok = tailLen >= 22 && fread(tail.data(), 1, tail.size(), f) == tail.size();
  long long eocd = -1;
  for (long long i = tailLen - 22; ok && i >= 0; i--)
    if (rd32(&tail[(size_t)i]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    fclose(f);
    return false;
  }
  const uint8_t* e = &tail[(size_t)eocd];
  uint32_t count = rd16(e + 10), cdSize = rd32(e + 12), cdOff = rd32(e + 16);
  if (count == 0xFFFF || cdOff == 0xFFFFFFFFu || (long long)cdOff + cdSize > size || cdSize > (64u << 20) || count > 20000) {
    fclose(f);
    return false;
  }
  std::vector<uint8_t> cd(cdSize);
  _fseeki64(f, cdOff, SEEK_SET);
  ok = fread(cd.data(), 1, cd.size(), f) == cd.size();
  fclose(f);
  size_t p = 0;
  for (uint32_t i = 0; ok && i < count; i++) {
    if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) return false;
    ZipEntry z;
    z.flags = rd16(&cd[p + 8]);
    z.method = rd16(&cd[p + 10]);
    z.crc = rd32(&cd[p + 16]);
    z.csize = rd32(&cd[p + 20]);
    z.usize = rd32(&cd[p + 24]);
    uint16_t n = rd16(&cd[p + 28]), x = rd16(&cd[p + 30]), c = rd16(&cd[p + 32]);
    z.offset = rd32(&cd[p + 42]);
    if (p + 46 + n + x + c > cd.size()) return false;
    z.name.assign((const char*)&cd[p + 46], n);
    p += 46 + (size_t)n + x + c;
    out.push_back(z);
  }
  if (!ok) return false;
  err.clear();
  return true;
}

bool zipRead(const std::string& zip, const ZipEntry& z, std::vector<uint8_t>& out, std::string& err) {
  err = "!badzip " + z.name;
  out.clear();
  if ((z.flags & 1) || (z.method != 0 && z.method != 8) || z.usize > (uint32_t)INT_MAX || z.csize > (uint32_t)INT_MAX ||
      (z.method == 0 && z.csize != z.usize))
    return false;
  FILE* f = ufopen(zip, "rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  long long size = _ftelli64(f);
  uint8_t h[30];
  _fseeki64(f, z.offset, SEEK_SET);
  bool ok = fread(h, 1, 30, f) == 30 && rd32(h) == 0x04034b50;
  long long data = (long long)z.offset + 30 + rd16(h + 26) + rd16(h + 28);
  ok = ok && data + z.csize <= size;
  std::vector<uint8_t> in;
  if (ok) {
    in.resize(z.csize);
    _fseeki64(f, data, SEEK_SET);
    ok = z.csize == 0 || fread(in.data(), 1, in.size(), f) == in.size();
  }
  fclose(f);
  if (!ok) return false;
  if (z.method == 0) {
    out.swap(in);
  } else {
    out.resize(z.usize);
    if (z.usize > 0) {
      int r = stbi_zlib_decode_noheader_buffer((char*)out.data(), (int)z.usize, (const char*)in.data(), (int)in.size());
      if (r != (int)z.usize) {
        out.clear();
        return false;
      }
    }
  }
  if (crc32Of(out.data(), out.size()) != z.crc) {
    out.clear();
    return false;
  }
  err.clear();
  return true;
}

// ---------------------------------------------------------------- Updater
bool Updater::liteBuild() { return !fileExists(exeDir() + "\\ai\\onnxruntime.dll"); }

ReleaseInfo Updater::latest() const {
  std::lock_guard<std::mutex> lk(m_);
  return rel_;
}
std::string Updater::error() const {
  std::lock_guard<std::mutex> lk(m_);
  return err_;
}
void Updater::fail(const std::string& e) {
  {
    std::lock_guard<std::mutex> lk(m_);
    err_ = e;
  }
  state_ = Failed;
}

void Updater::check(bool manual) {
  if (working() || state_ == Ready) return;
  poll();
  if (th_.joinable()) return;
  manual_ = manual;
  cancel_ = false;
  done_ = false;
  state_ = Checking;
  th_ = std::thread([this]() { run(false); });
}

void Updater::install() {
  if (state_ != Available && state_ != Failed) return;
  poll();
  if (th_.joinable()) return;
  ReleaseInfo r = latest();
  if (r.assetUrl.empty()) return;
  cancel_ = false;
  done_ = false;
  got_ = 0;
  total_ = r.assetSize;
  state_ = Downloading;
  th_ = std::thread([this]() { run(true); });
}

void Updater::poll() {
  if (done_ && th_.joinable()) {
    th_.join();
    done_ = false;
  }
}

void Updater::shutdown() {
  cancel_ = true;
  {
    std::lock_guard<std::mutex> lk(g_reqM);
    if (g_req) WinHttpCloseHandle(g_req);  // wakes a read that is waiting on the network
    g_req = nullptr;
  }
  if (th_.joinable()) th_.join();
}

void Updater::run(bool doInstall) {
  try {
    std::string err;
    if (!doInstall) {
      std::string body;
      if (!httpGet(updateFeedUrl(), &body, "", 2u << 20, nullptr, nullptr, cancel_, err)) {
        fail(err);
      } else {
        ReleaseInfo r;
        if (!parseRelease(body, liteBuild(), r, err)) {
          fail(err);
        } else {
          {
            std::lock_guard<std::mutex> lk(m_);
            rel_ = r;
            err_ = err;  // "!noasset": newer, but only by manual download
          }
          state_ = compareVersions(r.version, CR_VERSION_STR) > 0 ? Available : UpToDate;
        }
      }
    } else {
      ReleaseInfo r = latest();
      const std::string dir = exeDir(), stage = dir + "\\.update";
      removeTree(stage);  // leftovers of an interrupted attempt
      std::string zip = stage + "\\" + r.assetName, part = zip + ".part";
      std::vector<std::pair<std::string, std::string>> files;  // target, staged
      if (!mkdirs(stage) || !writeTextFile(stage + "\\probe", "x")) {
        fail("!nowrite");
      } else if (setHiddenU(stage), !httpGet(r.assetUrl, nullptr, part, 0, &got_, &total_, cancel_, err) ||
                 !replaceFileU(part, zip)) {
        fail(err.empty() ? "!nowrite" : err);
      } else {
        state_ = Installing;
        std::string sums, want, have;
        if (!httpGet(r.sumsUrl, &sums, "", 64 << 10, nullptr, nullptr, cancel_, err)) {
          fail(err);
          goto done;
        }
        {  // "<64 hex>  CircleRoom_Portable.zip"
          size_t pos = 0;
          while (pos < sums.size()) {
            size_t e = sums.find('\n', pos);
            std::string ln = sums.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? sums.size() : e + 1;
            while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
            size_t sp = ln.find(' ');
            if (sp == 64 && ln.size() > 66) {
              std::string name = ln.substr(ln.find_first_not_of(" *", sp));
              if (name == r.assetName) {
                want = ln.substr(0, 64);
                for (char& c : want) c = (char)tolower((unsigned char)c);
              }
            }
          }
        }
        if (want.empty() || !sha256File(zip, have) || have != want) {
          fail("!badhash");
          goto done;
        }
        std::vector<ZipEntry> entries;
        if (!zipList(zip, entries, err)) {
          fail(err);
          goto done;
        }
        // Stage every changed file first; nothing in the program folder changes until all are ready.
        for (const ZipEntry& z : entries) {
          if (cancel_) {
            fail("!cancelled");
            goto done;
          }
          if (z.name.empty() || z.name.back() == '/') continue;
          std::string rel;
          if (!zipEntryPath(z.name, rel)) {
            fail("!badzip " + z.name);
            goto done;
          }
          std::string target = dir + "\\" + rel;
          unsigned long long sz = 0;
          long long mt = 0;
          uint32_t crc = 0;
          if (fileInfo(target, sz, mt) && sz == z.usize && crcOfFile(target, crc) && crc == z.crc) continue;  // unchanged
          std::vector<uint8_t> data;
          if (!zipRead(zip, z, data, err)) {
            fail(err);
            goto done;
          }
          std::string staged = stage + "\\new\\" + rel;
          FILE* f = (mkdirs(dirOf(staged))) ? ufopen(staged, "wb") : nullptr;
          bool w = f && (data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size());
          if (f && fclose(f) != 0) w = false;
          if (!w) {
            fail("!nowrite");
            goto done;
          }
          files.push_back({target, staged});
        }
        // Swap in: the old file (even the running exe) is renamed aside first; undo all on any failure.
        {
          struct Moved {
            std::string target, old;
          };
          std::vector<Moved> moved;
          bool ok = true;
          unsigned long long tag = GetTickCount64();
          for (const auto& fl : files) {
            std::wstring t = utf8ToWide(fl.first);
            std::string old;
            if (fileExists(fl.first)) {
              old = fl.first + "." + std::to_string(tag) + ".old";
              if (!MoveFileExW(t.c_str(), utf8ToWide(old).c_str(), MOVEFILE_REPLACE_EXISTING)) {
                ok = false;
                break;
              }
            } else if (!mkdirs(dirOf(fl.first))) {
              ok = false;
              break;
            }
            if (!MoveFileExW(utf8ToWide(fl.second).c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
              if (!old.empty()) MoveFileExW(utf8ToWide(old).c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING);
              ok = false;
              break;
            }
            moved.push_back({fl.first, old});
          }
          if (!ok) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
              std::wstring t = utf8ToWide(it->target);
              DeleteFileW(t.c_str());
              if (!it->old.empty()) MoveFileExW(utf8ToWide(it->old).c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            fail("!install " + std::to_string(GetLastError()));
            goto done;
          }
        }
        deleteFileU(zip);
        state_ = Ready;
      }
    done:;
      if (state_ != Ready) removeTree(stage);  // a failed or cancelled attempt leaves nothing behind
    }
  } catch (const std::exception& e) {
    fail(std::string("!install ") + e.what());
  }
  done_ = true;
}

bool Updater::restart(const std::string& reopen) {
  std::wstring exe = utf8ToWide(exeDir() + "\\CircleRoom.exe");
  std::wstring cmd = L"\"" + exe + L"\"";
  if (!reopen.empty()) cmd += L" \"" + utf8ToWide(reopen) + L"\"";
  STARTUPINFOW si;
  std::memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  std::wstring dir = utf8ToWide(exeDir());
  if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void Updater::cleanupOld() {
  std::string dir = exeDir();
  if (isDirectory(dir + "\\.update")) removeTree(dir + "\\.update");
  for (const char* sub : {"", "\\ai", "\\LICENSES"}) {
    std::string d = dir + sub;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(utf8ToWide(d + "\\*.old").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) continue;
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(utf8ToWide(d + "\\" + wideToUtf8(fd.cFileName)).c_str());
    } while (FindNextFileW(f, &fd));
    FindClose(f);
  }
}
