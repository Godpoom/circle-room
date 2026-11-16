// "Make your font": each character is drawn in the app (or imported from an image) on a 256 px
// cell, traced into smooth outlines and written as a TrueType font. The font is usable in Circle
// Room at once and can be installed for this Windows user (no admin rights needed).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

constexpr int kGlyphCell = 256;      // drawing cell, pixels
constexpr int kGlyphBaseline = 198;  // baseline, pixels from the top of the cell
constexpr int kGlyphXHeight = 82;    // guides, pixels from the top
constexpr int kGlyphCapHeight = 35;
constexpr int kGlyphDescender = 245;

// The characters offered, in grid order: Thai consonants, vowels, tone marks and digits, then
// Latin letters, digits and punctuation.
const std::vector<uint32_t>& fontMakerChars();
bool isThaiMark(uint32_t cp);  // above / below vowels and tone marks: zero width, sit on the letter before

struct FontProject {
  std::string name;   // family name (any script)
  std::string dir;    // %LOCALAPPDATA%\CircleRoom\fontprojects\<safe name>
  // One 8-bit coverage bitmap per character (empty = not drawn yet), kGlyphCell x kGlyphCell.
  std::vector<std::vector<uint8_t>> ink;
};
std::vector<std::string> listFontProjects();
bool openFontProject(const std::string& name, FontProject& p, std::string& err);  // creates it if new
bool saveGlyph(const FontProject& p, size_t index, std::string& err);
int drawnGlyphs(const FontProject& p);

// Places an image (dark ink on a light background, or ink with transparency) into a cell.
bool importGlyphImage(const std::string& file, std::vector<uint8_t>& ink, std::string& err);
// A printable sheet of empty cells (with their characters) to fill by hand, and its import.
bool writeTemplateSheet(const std::string& pngPath, std::string& err);
int importTemplateSheet(const std::string& file, FontProject& p, std::string& err);  // glyphs filled

// Traces the drawn glyphs and writes "<userFontDir>\<name>.ttf". Returns the font file.
bool buildFont(const FontProject& p, std::string& ttfPath, std::string& err);
// Installs a font file for the current Windows user (Windows 10 1809 and later).
bool installFontForUser(const std::string& ttfPath, const std::string& family, std::string& err);

// Exposed for the self-test: a TrueType file from contours in font units (y up, 1000 per em).
struct FmGlyph {
  uint32_t cp = 0;
  int advance = 0;
  std::vector<std::vector<std::pair<int, int>>> contours;
};
std::vector<uint8_t> writeTrueType(const std::string& family, const std::vector<FmGlyph>& glyphs);
// Iso-line tracing of an 8-bit coverage bitmap into closed contours (pixels), correctly wound.
std::vector<std::vector<std::pair<float, float>>> traceBitmap(const std::vector<uint8_t>& ink, int w, int h);
