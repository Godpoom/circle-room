// The update dialog: what is new, download progress, restart.
#pragma once
#include <functional>
#include <string>

#include "updater.h"

struct UpdateHost {
  float scale = 1.f;
  bool* autoCheck = nullptr;     // settings: check when the app starts
  std::string* skip = nullptr;   // settings: version the user chose to skip
  bool canRestart = true;        // false while an export is being written
  std::function<void()> restart;
};

void drawUpdateDialog(Updater& u, UpdateHost& h, bool& open);
std::string updateErrorText(const std::string& e);
