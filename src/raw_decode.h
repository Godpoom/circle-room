// Camera RAW files (CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2, ...) through LibRaw.
// The editor gets the full sensor data as 16-bit linear RGB with the camera's white balance, so
// highlights and shadows keep the latitude a JPEG has already thrown away. The AI paths get the
// camera's embedded preview JPEG when it is large enough (fast), else a half-size decode.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "image_io.h"

bool isRawFile(const std::string& path);  // by extension
const char* const* rawExtensions();       // "cr2", "nef", ... ; ends with nullptr

// Full quality for the editor and export: out.is16, linear RGB, out.orientation from the file.
bool loadRawImage(const std::string& path, int maxDim, ImageData& out, std::string& err);

// sRGB 8-bit, long edge at least minEdge where the file allows, NOT yet rotated: `orientation`
// is the EXIF code to apply. fullW/fullH are the unrotated output size of the RAW itself.
struct RawPreview {
  int w = 0, h = 0, orientation = 1, fullW = 0, fullH = 0;
  int64_t shotTime = 0;  // capture time, seconds since 1970 (0 = unknown)
  std::vector<uint8_t> rgb;
};
bool loadRawPreview(const std::string& path, int minEdge, RawPreview& out, std::string& err);
