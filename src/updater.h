// In-app updates from the project's GitHub releases.
// check(): asks GitHub for the latest release (one small HTTPS request, no personal data).
// install(): downloads the zip that matches this build (Portable with AI, or Lite), checks its
// SHA-256 against the release's SHA256SUMS.txt, unpacks only the files that changed into a
// staging folder, then swaps them in (the running exe and DLLs are renamed to *.old, which
// Windows allows; they are deleted on the next start). Any failure rolls the swap back.
// Settings, presets and photos are never touched (they are not in the release zip).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ReleaseInfo {
  std::string version;   // "1.3.0"
  std::string notes;     // release notes, plain text
  std::string pageUrl;   // release page (manual download)
  std::string assetName, assetUrl, sumsUrl;
  long long assetSize = 0;
};

class Updater {
 public:
  enum State { Idle, Checking, UpToDate, Available, Downloading, Installing, Ready, Failed };
  ~Updater() { shutdown(); }

  void check(bool manual);  // worker thread
  void install();           // Available -> Downloading -> Installing -> Ready (or Failed)
  void cancel() { cancel_ = true; }
  void poll();              // UI thread: joins a finished worker
  void shutdown();

  State state() const { return state_; }
  bool working() const { State s = state_; return s == Checking || s == Downloading || s == Installing; }
  bool wasManual() const { return manual_; }
  float progress() const { long long t = total_; return t > 0 ? (float)((double)got_ / t) : 0.f; }
  long long bytesGot() const { return got_; }
  long long bytesTotal() const { return total_; }
  ReleaseInfo latest() const;
  std::string error() const;
  bool restart(const std::string& reopen);  // starts the new exe; the caller then closes

  static void cleanupOld();  // on start: removes *.old and the staging folder
  static bool liteBuild();   // no AI runtime next to the exe

 private:
  void run(bool doInstall);
  void fail(const std::string& e);
  std::thread th_;
  std::atomic<State> state_{Idle};
  std::atomic<bool> cancel_{false}, manual_{false}, done_{false};
  std::atomic<long long> got_{0}, total_{0};
  mutable std::mutex m_;
  ReleaseInfo rel_;
  std::string err_;
};

// ---- pieces, exposed for the self-test
int compareVersions(const std::string& a, const std::string& b);  // <0, 0, >0
bool parseRelease(const std::string& json, bool lite, ReleaseInfo& out, std::string& err);
bool zipEntryPath(const std::string& name, std::string& rel);      // "CircleRoom/ai/x.dll" -> "ai\\x.dll"; rejects traversal
std::string sha256Hex(const void* data, size_t n);
bool sha256File(const std::string& path, std::string& hex);
uint32_t crc32Of(const void* data, size_t n, uint32_t crc = 0);
std::string updateFeedUrl();

struct ZipEntry {
  std::string name;
  uint32_t crc = 0, csize = 0, usize = 0, offset = 0;
  uint16_t method = 0, flags = 0;
};
bool zipList(const std::string& zip, std::vector<ZipEntry>& out, std::string& err);
bool zipRead(const std::string& zip, const ZipEntry& e, std::vector<uint8_t>& out, std::string& err);
