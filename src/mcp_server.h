// "CircleRoom.exe --mcp": a Model Context Protocol server over stdio, so AI agents (Claude Code,
// Codex, Antigravity, Cursor, Gemini CLI, VS Code, ...) can edit photos with every Circle Room
// tool and see the result as an image after each step. Runs headless in the calling process;
// edits are the same non-destructive .pedit files the app reads.
#pragma once
#include <string>
#include <vector>

// Serves until stdin closes. Needs a current OpenGL 3.3 context (a hidden window).
int mcpServe(std::string& err);

// Ready-to-paste setup for each AI app, with this exe's path filled in (the "Connect AI" dialog).
struct McpClient {
  std::string name;
  int how;                            // i18n key: where the snippet goes
  std::vector<std::string> snippets;  // one or more alternatives to copy
};
std::vector<McpClient> mcpClients();
