#include "agent_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

#include "mcp_json.h"
#include "platform_win.h"

std::string agentLogDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::string base = (n > 0 && n < MAX_PATH) ? wideToUtf8(buf) : exeDir();
  return base + "\\CircleRoom\\agents";
}

static bool ensureAgentDir() {
  std::string d = agentLogDir();
  ensureDir(dirOf(d));
  return ensureDir(d) || isDirectory(d);
}

std::string agentClientName(const std::string& raw) {
  std::string l = raw;
  for (char& c : l) c = (char)tolower((unsigned char)c);
  struct M { const char* has; const char* name; };
  static const M kNames[] = {{"claude-code", "Claude Code"}, {"claude code", "Claude Code"},
                             {"claude-ai", "Claude Desktop"}, {"claude", "Claude"},
                             {"codex", "Codex"},             {"opencode", "OpenCode"},
                             {"gemini", "Gemini CLI"},       {"cursor", "Cursor"},
                             {"windsurf", "Windsurf"},       {"antigravity", "Antigravity"},
                             {"visual studio code", "VS Code"}, {"vscode", "VS Code"},
                             {"qwen", "Qwen Code"},          {"kimi", "Kimi Code"},
                             {"deepseek", "DeepSeek TUI"},   {"codewhale", "DeepSeek TUI"},
                             {"mimo", "MiMo Code"},          {"zcode", "Z Code"},
                             {"z-code", "Z Code"},           {"z code", "Z Code"},
                             {"zhipu", "Z Code"}};
  for (const M& m : kNames)
    if (l.find(m.has) != std::string::npos) return m.name;
  if (raw.empty()) return "AI agent";
  return raw.size() > 40 ? raw.substr(0, 40) : raw;
}

// ---------------------------------------------------------------- writer
void AgentLog::open(const std::string& client) {
  if (!ensureAgentDir()) return;
  path_ = agentLogDir() + "\\" + std::to_string(GetCurrentProcessId()) + ".log";
  deleteFileU(path_);  // a recycled pid's leftovers
  Json j = Json::object();
  j.set("e", "hello");
  j.set("client", agentClientName(client));
  line(json::dump(j));
}

// Opened, appended and closed per event: closing is what makes the folder watcher fire promptly.
void AgentLog::line(const std::string& json) {
  if (path_.empty()) return;
  FILE* f = ufopen(path_, "ab");
  if (!f) return;
  fwrite(json.data(), 1, json.size(), f);
  fputc('\n', f);
  fclose(f);
}

void AgentLog::call(int id, const std::string& tool, const std::string& photo, const std::string& detail) {
  Json j = Json::object();
  j.set("e", "call");
  j.set("id", id);
  j.set("tool", tool);
  j.set("photo", photo);
  j.set("detail", detail);
  line(json::dump(j));
}

void AgentLog::done(int id, const std::string& tool, bool ok, const std::string& photo,
                    const std::vector<AgentChange>& changes, const std::string& msg) {
  Json j = Json::object();
  j.set("e", "done");
  j.set("id", id);
  j.set("tool", tool);
  j.set("ok", ok);
  j.set("photo", photo);
  j.set("msg", msg);
  Json c = Json::array();
  for (const AgentChange& ch : changes) {
    Json e = Json::array();
    e.push(ch.key);
    e.push(ch.before);
    e.push(ch.after);
    c.push(e);
  }
  j.set("changes", c);
  line(json::dump(j));
}

void AgentLog::close() {
  if (path_.empty()) return;
  line("{\"e\":\"bye\"}");
  path_.clear();
}

// ---------------------------------------------------------------- reader
const AgentEvent* AgentSession::current() const {
  for (auto it = events.rbegin(); it != events.rend(); ++it)
    if (it->running) return &*it;
  return nullptr;
}

// Still running, and still a Circle Room process (pids are recycled).
static bool agentAlive(unsigned long pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return false;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  if (alive) {
    wchar_t img[MAX_PATH], self[MAX_PATH];
    DWORD n = MAX_PATH;
    DWORD m = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (QueryFullProcessImageNameW(h, 0, img, &n) && m > 0 && m < MAX_PATH) {
      const wchar_t* a = wcsrchr(img, L'\\');
      const wchar_t* b = wcsrchr(self, L'\\');
      alive = _wcsicmp(a ? a + 1 : img, b ? b + 1 : self) == 0;
    }
  }
  CloseHandle(h);
  return alive;
}

void AgentWatch::start(std::function<void()> wake) {
  if (running_) return;
  wake_ = std::move(wake);
  stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  running_ = true;
  th_ = std::thread([this]() { run(); });
}

void AgentWatch::stop() {
  if (!running_) return;
  SetEvent((HANDLE)stopEv_);
  if (th_.joinable()) th_.join();
  CloseHandle((HANDLE)stopEv_);
  stopEv_ = nullptr;
  running_ = false;
}

void AgentWatch::run() {
  ensureAgentDir();
  std::wstring dir = utf8ToWide(agentLogDir());
  HANDLE ch = FindFirstChangeNotificationW(
      dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
  for (;;) {
    HANDLE hs[2] = {(HANDLE)stopEv_, ch};
    DWORD count = ch != INVALID_HANDLE_VALUE ? 2 : 1;
    // While an agent is connected, look every 1.5 s anyway (catches a killed server);
    // with nobody connected, sleep until a file appears.
    DWORD timeout = live_ > 0 || ch == INVALID_HANDLE_VALUE ? 1500 : INFINITE;
    DWORD r = WaitForMultipleObjects(count, hs, FALSE, timeout);
    if (r == WAIT_OBJECT_0) break;
    if (r == WAIT_OBJECT_0 + 1) FindNextChangeNotification(ch);
    if (r == WAIT_FAILED) Sleep(1500);
    dirty_ = true;
    if (wake_) wake_();
  }
  if (ch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ch);
}

bool AgentWatch::anyAlive() const {
  for (const AgentSession& s : s_)
    if (s.alive) return true;
  return false;
}

static void markGone(AgentSession& s, double now) {
  s.alive = false;
  s.goneAt = now;
  for (AgentEvent& e : s.events)
    if (e.running) {
      e.running = false;
      e.ok = false;
    }
  deleteFileU(s.file);
}

bool AgentWatch::poll(double now, std::vector<AgentEvent>& finished) {
  bool changed = false;
  // Disconnected agents stay on screen for a few seconds, then go.
  size_t before = s_.size();
  s_.erase(std::remove_if(s_.begin(), s_.end(), [now](const AgentSession& s) { return !s.alive && now - s.goneAt > 8.0; }),
           s_.end());
  changed |= s_.size() != before;
  if (!dirty_.exchange(false)) return changed;

  std::string dir = agentLogDir();
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileW(utf8ToWide(dir + "\\*.log").c_str(), &fd);
  std::vector<unsigned long> seen;
  if (f != INVALID_HANDLE_VALUE) {
    do {
      wchar_t* end = nullptr;
      unsigned long pid = wcstoul(fd.cFileName, &end, 10);
      if (!pid || !end || _wcsicmp(end, L".log") != 0) continue;
      seen.push_back(pid);
      auto it = std::find_if(s_.begin(), s_.end(), [pid](const AgentSession& s) { return s.pid == pid; });
      std::string file = dir + "\\" + wideToUtf8(fd.cFileName);
      if (it == s_.end()) {
        if (!agentAlive(pid)) {  // left behind by a server that was killed while we were closed
          deleteFileU(file);
          continue;
        }
        AgentSession ns;
        ns.pid = pid;
        ns.file = file;
        ns.client = "AI agent";
        s_.push_back(ns);
        it = s_.end() - 1;
        changed = true;
      }
      AgentSession& s = *it;
      if (!s.alive) continue;
      long long size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      if (size < s.offset) s.offset = 0, s.partial.clear();  // rewritten
      if (size > s.offset) {
        FILE* fp = ufopen(file, "rb");
        if (fp) {
          long long want = std::min<long long>(size - s.offset, 4 << 20);
          std::string buf((size_t)want, '\0');
          _fseeki64(fp, s.offset, SEEK_SET);
          size_t got = fread(&buf[0], 1, buf.size(), fp);
          fclose(fp);
          s.offset += (long long)got;
          s.partial.append(buf.data(), got);
        }
      }
      size_t nl;
      while ((nl = s.partial.find('\n')) != std::string::npos) {
        std::string ln = s.partial.substr(0, nl);
        s.partial.erase(0, nl + 1);
        Json j;
        if (!json::parse(ln, j) || !j.isObj()) continue;
        std::string e = j.str("e");
        changed = true;
        if (e == "hello") {
          s.client = j.str("client", "AI agent");
        } else if (e == "call") {
          AgentEvent ev;
          ev.id = (int)j.num("id", 0);
          ev.tool = j.str("tool");
          ev.photo = j.str("photo");
          ev.detail = j.str("detail");
          s.events.push_back(ev);
        } else if (e == "done") {
          int id = (int)j.num("id", 0);
          AgentEvent* ev = nullptr;
          for (auto r = s.events.rbegin(); r != s.events.rend(); ++r)
            if (r->id == id && r->running) {
              ev = &*r;
              break;
            }
          if (!ev) {
            s.events.emplace_back();
            ev = &s.events.back();
            ev->id = id;
          }
          ev->running = false;
          ev->tool = j.str("tool", ev->tool);
          ev->photo = j.str("photo", ev->photo);
          ev->ok = j.flag("ok", true);
          ev->msg = j.str("msg");
          ev->changes.clear();
          if (const Json* c = j.get("changes"))
            for (const Json& x : c->a)
              if (x.isArr() && x.a.size() == 3) ev->changes.push_back({x.a[0].s, x.a[1].s, x.a[2].s});
          finished.push_back(*ev);
        } else if (e == "bye") {
          markGone(s, now);
          break;
        }
      }
      if (s.events.size() > 40) s.events.erase(s.events.begin(), s.events.end() - 40);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
  }
  for (AgentSession& s : s_) {
    if (!s.alive) continue;
    bool present = std::find(seen.begin(), seen.end(), s.pid) != seen.end();
    if (!present || !agentAlive(s.pid)) {
      markGone(s, now);
      changed = true;
    }
  }
  int live = 0;
  for (const AgentSession& s : s_) live += s.alive;
  live_ = live;
  return changed;
}
