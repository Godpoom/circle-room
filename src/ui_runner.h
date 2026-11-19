// A small hidden game (click the logo at the top left 11 times): the Circle Room mark runs
// through a darkroom and jumps over tripods, lamps and film canisters. Space / Up / click jumps,
// Esc closes. The best score is kept in %LOCALAPPDATA%\CircleRoom\run_best.txt.
#pragma once
#include <vector>

struct RunObstacle {
  float x, w, h, lift;  // lift > 0: hangs in the air (pass under it, or time the jump)
  int kind;             // 0 tripod, 1 film canisters, 2 hanging lamp
};

struct Runner {
  bool open = false, request = false;
  int state = 0;  // 0 ready, 1 running, 2 crashed
  float y = 0, vy = 0, speed = 0, dist = 0, nextGap = 0, ground = 0, t = 0, crashT = 0;
  bool holding = false;
  int best = -1, score = 0;
  unsigned rng = 12345;
  std::vector<RunObstacle> obs;
};

void runnerOpen(Runner& r);  // shows it on the next frame
// Draws the game window and steps the game; returns true while it is open (the main loop keeps
// rendering frames then, since the game animates).
bool drawRunner(Runner& r, float scale);
// Logo clicks: true on the 11th click in a row (each within 1.2 s of the last).
bool runnerLogoClick(double now);
// Self-test: (idle player score, bot player score); true when the idle one crashed and the bot survived 2 minutes.
bool runnerSimCheck(int& idleScore, int& botScore);
