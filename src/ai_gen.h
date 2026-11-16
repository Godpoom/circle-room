// Generative edits through an image model on OpenRouter (see openrouter.h): fill a painted area
// from a prompt, remove or move something, rebuild blown highlights, or make a new picture.
// The result never touches the photo: it becomes RGBA layers (PNG next to the photo in
// ".circleroom_layers") that the compose page stacks, blends and can undo like any layer.
#pragma once
#include <string>
#include <vector>

#include "heal.h"  // HealStroke: a painted stroke in oriented uv
#include "openrouter.h"

enum GenTask { GEN_EDIT, GEN_REMOVE, GEN_HIGHLIGHT, GEN_MOVE, GEN_NEW, GEN_TASK_COUNT };

struct GenInput {
  int task = GEN_EDIT;
  std::string photo;            // the open photo (not needed for GEN_NEW)
  std::string prompt;           // the user's words (any language)
  std::string key;              // OpenRouter API key
  orouter::Model model;
  std::vector<HealStroke> mask;  // painted area; empty = whole photo (edit) / auto (highlights)
  float aspect = 1.f;           // GEN_NEW: width / height wanted
  bool hiRes = false;           // ask for 2K pictures (sharper, costs more); 1K otherwise
};

struct GenLayer {
  std::string file;          // PNG written for the layer
  float cx = 0.5f, cy = 0.5f;  // centre, 0..1 of the photo
  float scale = 1.f;         // width as a share of the photo width
  bool photoKind = false;    // plain photo layer (no alpha), e.g. a new picture placed on top
};

struct GenOutput {
  std::vector<GenLayer> layers;  // bottom first
  std::string newImage;          // GEN_NEW: the saved picture
  double cost = -1;              // USD
  std::string err;
};

// Blocking; run on a worker thread. orouter::cancel() stops it.
bool runGen(const GenInput& in, GenOutput& out);

// Pieces for the self-test.
void rasterMask(const std::vector<HealStroke>& strokes, int W, int H, std::vector<float>& m);
void boxBlur(std::vector<float>& m, int W, int H, int r);
// Crop around the mask with context, close to aspect R, inside W x H.
void contextRect(int bx0, int by0, int bx1, int by1, int W, int H, float R, int& x, int& y, int& w, int& h);
std::string genPrompt(int task, const std::string& user, bool hasMask);
std::string picturesAiDir();
std::string picturesSubDir(const char* name);  // Pictures\<name>, created
// Self-test: answer requests locally instead of calling OpenRouter (nullptr = the real thing).
using GenHook = bool (*)(const orouter::Request& rq, orouter::Result& out, std::string& err);
void setGenHook(GenHook h);
