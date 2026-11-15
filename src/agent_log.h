// Live link between AI agents and the open Circle Room window.
// Each MCP session (CircleRoom.exe --mcp, started by Claude Code, Codex, ...) appends one JSON
// line per event to %LOCALAPPDATA%\CircleRoom\agents\<pid>.log. The app watches that folder
// (a change notification wakes it; nothing runs while idle) and shows who is connected, which
// tool is running and what each step changed.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

std::string agentLogDir();

// What a tool call changed in a photo's edit: one entry per slider or per group
// (key "crop", "hsl", "grade", "region:sky", "layer", "heals", "recover", or a slider key).
struct AgentChange {
  std::string key, before, after;
};

// ---------------------------------------------------------------- writer (MCP server)
class AgentLog {
 public:
  ~AgentLog() { close(); }
  void open(const std::string& client);
  void call(int id, const std::string& tool, const std::string& photo, const std::string& detail);
  void done(int id, const std::string& tool, bool ok, const std::string& photo, const std::vector<AgentChange>& changes,
            const std::string& msg);
  void close();  // "bye": the app shows the agent as disconnected

 private:
  void line(const std::string& json);
  std::string path_;
};

// ---------------------------------------------------------------- reader (app)
struct AgentEvent {
  int id = 0;
  std::string tool, photo, detail, msg;
  std::vector<AgentChange> changes;
  bool ok = true, running = true;
};

struct AgentSession {
  unsigned long pid = 0;
  std::string client, file, partial;
  long long offset = 0;
  bool alive = true;
  double goneAt = 0;             // seconds (glfw clock) when it disconnected
  std::vector<AgentEvent> events;  // newest last, at most 40
  const AgentEvent* current() const;  // the running call, if any
};

class AgentWatch {
 public:
  ~AgentWatch() { stop(); }
  // wake() is called from the watcher thread when the folder changes (post an empty event).
  void start(std::function<void()> wake);
  void stop();
  // On the UI thread when woken (or each frame: cheap when nothing changed). Appends tool calls
  // that finished since the last call to `finished`; true when anything on screen changed.
  bool poll(double now, std::vector<AgentEvent>& finished);
  const std::vector<AgentSession>& sessions() const { return s_; }
  bool anyAlive() const;
  void refresh() { dirty_ = true; }  // read the folder on the next poll (tests; the watcher does this)

 private:
  void run();
  std::vector<AgentSession> s_;
  std::thread th_;
  void* stopEv_ = nullptr;
  std::atomic<bool> dirty_{true}, running_{false};
  std::atomic<int> live_{0};
  std::function<void()> wake_;
};

// Pretty name for an MCP clientInfo.name ("claude-code" -> "Claude Code").
std::string agentClientName(const std::string& raw);
