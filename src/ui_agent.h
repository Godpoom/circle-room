// "AI is working" card: which AI app is connected over MCP, the tool it is running right now,
// and what each step changed, shown live over the editor (and the cull view).
#pragma once
#include <functional>
#include <string>

#include "agent_log.h"

struct AgentHost {
  float scale = 1.f;
  float bottomGap = 0.f;  // room to leave at the bottom right (batch progress card)
  std::string openPath;   // photo open in the editor, "" when none
  std::function<void(const std::string&)> openPhoto;
};

class AgentPanel {
 public:
  AgentWatch watch;
  bool follow = true;  // open the photo the agent is editing

  void draw(AgentHost& h);
  bool connected() const { return watch.anyAlive(); }
  bool animating() const;  // a tool is running, or a card is fading out: keep redrawing
  std::string clientName() const;
  void toggle() { hidden_ = !hidden_; }
  void show() { hidden_ = false; }

 private:
  bool hidden_ = false, collapsed_ = false;
  size_t seenSessions_ = 0;
};

// Display names (current language) for a tool and for a changed setting.
const char* agentToolName(const std::string& tool);
std::string agentChangeText(const AgentChange& c);
