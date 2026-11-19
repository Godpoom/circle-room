// Text and vector layers. Their content (the words and style, or the SVG markup) lives in a small
// immutable file named by its hash in a hidden ".circleroom_layers" folder next to the photo;
// ImageLayer::path points at it, so undo can return to any earlier version and the photo itself
// is never touched. Both kinds render on the CPU to colour + alpha and then behave like photo
// layers: move, scale, rotate, blend, erase, undo.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---- content files
std::string layerContentDir(const std::string& photo);
// Writes data as "<hash>.<ext>" (once; identical content reuses the file). ext: "crtext" or "svg".
bool writeLayerContent(const std::string& photo, const std::string& data, const char* ext, std::string& path, std::string& err);
bool readLayerContent(const std::string& path, std::string& data, std::string& err);  // at most 4 MB
bool isTextContent(const std::string& path);
bool isVectorContent(const std::string& path);

// ---- text
struct TextSpec {
  std::string text = "Text";  // UTF-8, '\n' between lines
  std::string font = "Sarabun";
  uint32_t color = 0xFFFFFFFF;  // 0xRRGGBBAA
  int bold = 0, italic = 0;
  int align = 1;            // 0 left, 1 centre, 2 right
  float spacing = 100.f;    // line spacing, % of the font's line height
  float outline = 0.f;      // outline width, % of the font size (0..30)
  uint32_t outlineColor = 0x000000FF;
};
std::string serializeText(const TextSpec& t);
bool parseText(const std::string& data, TextSpec& t);
// Renders at a fixed internal size (long edge <= 4096). rgb: sRGB, alpha: 0..255, both w*h.
bool renderText(const TextSpec& t, std::vector<uint8_t>& rgb, std::vector<uint8_t>& alpha, int& w, int& h, std::string& err);
// The width / height renderText would give, at a fraction of the cost (for the UI thread).
bool measureText(const TextSpec& t, float& aspect);

// One line of sample text in a font, for the font list: alpha only (white ink), exactly h pixels
// tall, at most maxW wide (cut off on the right).
bool renderFontSample(const std::string& family, const std::string& sample, int h, int maxW, std::vector<uint8_t>& alpha,
                      int& w);

// ---- vector drawing (SVG: paths, shapes, fills, strokes, gradients; no text or filters)
bool renderSvg(const std::string& svg, int maxDim, std::vector<uint8_t>& rgb, std::vector<uint8_t>& alpha, int& w, int& h,
               std::string& err);

// ---- fonts: bundled (exe\fonts), made in Circle Room (%LOCALAPPDATA%\CircleRoom\fonts) and installed
struct FontEntry {
  std::string family;
  bool thai = false;  // has Thai letters
};
void initFonts();                              // once per process; cheap after the first call
void reloadFonts();                            // after a font was made or installed
const std::vector<FontEntry>& fontList();      // sorted by family name
bool fontAvailable(const std::string& family);
std::string userFontDir();
bool loadPrivateFont(const std::string& file);  // usable in this app from now on

// Colours as "#RRGGBB" or "#RRGGBBAA".
std::string colorHex(uint32_t rgba);
bool parseColorHex(const std::string& s, uint32_t& rgba);
