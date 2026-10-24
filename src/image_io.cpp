#include "image_io.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <new>

#include "platform_win.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_MAX_DIMENSIONS (1 << 17)  // 131072 px per side; stb also rejects w*h*bpp beyond 2 GB
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Reads the EXIF orientation tag (0x0112) from a JPEG's APP1 segment.
static int exifOrientation(const uint8_t* d, size_t n) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
  size_t i = 2;
  while (i + 4 <= n) {
    if (d[i] != 0xFF) return 1;
    uint8_t marker = d[i + 1];
    if (marker == 0xD9 || marker == 0xDA) break;
    size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
    if (len < 2 || i + 2 + len > n) return 1;
    if (marker == 0xE1 && len >= 16 && std::memcmp(d + i + 4, "Exif\0\0", 6) == 0) {
      const uint8_t* t = d + i + 10;
      size_t tn = len - 8;
      bool le = t[0] == 'I';
      auto r16 = [&](size_t o) -> unsigned { return le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]); };
      auto r32 = [&](size_t o) -> size_t {
        return le ? ((size_t)t[o] | ((size_t)t[o + 1] << 8) | ((size_t)t[o + 2] << 16) | ((size_t)t[o + 3] << 24))
                  : (((size_t)t[o] << 24) | ((size_t)t[o + 1] << 16) | ((size_t)t[o + 2] << 8) | (size_t)t[o + 3]);
      };
      size_t ifd = r32(4);
      if (ifd + 2 > tn) return 1;
      unsigned count = r16(ifd);
      for (unsigned k = 0; k < count; k++) {
        size_t e = ifd + 2 + k * 12;
        if (e + 12 > tn) break;
        if (r16(e) == 0x0112) {
          unsigned v = r16(e + 8);
          return (v >= 1 && v <= 8) ? (int)v : 1;
        }
      }
      return 1;
    }
    i += 2 + len;
  }
  return 1;
}

// Photos larger than this are refused before any pixel buffer is allocated (a small file can
// claim a huge size; 500 MP is far beyond any camera, panoramas included).
static const uint64_t kMaxPixels = 500ull * 1000 * 1000;
static const uint64_t kMaxSmallPixels = 64ull * 1024 * 1024;  // thumbnails and AI caches

// File size fits stb's int length, and the header's dimensions are sane and within maxPixels.
static bool checkImage(const unsigned char* d, size_t n, uint64_t maxPixels, std::string& err) {
  if (n == 0 || n > (size_t)INT_MAX) {
    err = "file too large";
    return false;
  }
  int w = 0, h = 0, c = 0;
  if (!stbi_info_from_memory(d, (int)n, &w, &h, &c)) return true;  // let the decoder report the format error
  if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > maxPixels) {
    err = "image dimensions out of range";
    return false;
  }
  return true;
}

template <class T>
static void halve(std::vector<T>& px, int& w, int& h) {
  int nw = std::max(1, w / 2), nh = std::max(1, h / 2);  // a 1-px-wide strip stays 1 px wide
  std::vector<T> out((size_t)nw * nh * 3);
  for (int y = 0; y < nh; y++)
    for (int x = 0; x < nw; x++)
      for (int c = 0; c < 3; c++) {
        int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
        int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
        auto at = [&](int xx, int yy) { return (unsigned)px[((size_t)yy * w + xx) * 3 + c]; };
        out[((size_t)y * nw + x) * 3 + c] = (T)((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
      }
  px.swap(out);
  w = nw;
  h = nh;
}

static bool loadImageImpl(const std::string& path, int maxDim, ImageData& out, std::string& err) {
  unsigned long long fsize = 0;
  long long mtime = 0;
  if (fileInfo(path, fsize, mtime) && fsize > (unsigned long long)INT_MAX) { err = "file too large"; return false; }
  std::vector<unsigned char> file;
  if (!readFileBytes(path, file)) { err = "cannot read file"; return false; }
  if (!checkImage(file.data(), file.size(), kMaxPixels, err)) return false;
  maxDim = std::max(1, maxDim);
  out = ImageData();
  out.orientation = exifOrientation(file.data(), file.size());
  int w = 0, h = 0, n = 0;
  if (stbi_is_16_bit_from_memory(file.data(), (int)file.size())) {
    stbi_us* p = stbi_load_16_from_memory(file.data(), (int)file.size(), &w, &h, &n, 3);
    if (!p) { err = stbi_failure_reason(); return false; }
    std::vector<unsigned char>().swap(file);
    // Linearize on the CPU so GPU mipmaps are filtered in linear light.
    std::vector<uint16_t> lut(65536);
    for (int i = 0; i < 65536; i++) {
      double c = i / 65535.0;
      double l = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
      lut[i] = (uint16_t)std::lround(l * 65535.0);
    }
    out.px16.resize((size_t)w * h * 3);
    for (size_t i = 0; i < out.px16.size(); i++) out.px16[i] = lut[p[i]];
    stbi_image_free(p);
    out.is16 = true;
    while (w > maxDim || h > maxDim) halve(out.px16, w, h);
  } else {
    stbi_uc* p = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 3);
    if (!p) { err = stbi_failure_reason(); return false; }
    std::vector<unsigned char>().swap(file);
    out.px8.assign(p, p + (size_t)w * h * 3);
    stbi_image_free(p);
    while (w > maxDim || h > maxDim) halve(out.px8, w, h);
  }
  out.w = w;
  out.h = h;
  return true;
}

bool loadImage(const std::string& path, int maxDim, ImageData& out, std::string& err) {
  try {
    return loadImageImpl(path, maxDim, out, err);
  } catch (const std::bad_alloc&) {  // runs on worker threads: an exception there would end the app
    out = ImageData();
    err = "not enough memory for this image";
    return false;
  }
}

static void writeToFile(void* ctx, void* data, int size) { fwrite(data, 1, (size_t)size, (FILE*)ctx); }

// Images are encoded into "<name>.part" and renamed when complete, so a failed export never
// leaves a broken file behind or destroys the one it was replacing.
static bool finishPart(const std::string& part, const std::string& path, bool ok, std::string& err) {
  if (ok && replaceFileU(part, path)) return true;
  deleteFileU(part);
  if (err.empty()) err = "write failed";
  return false;
}

bool writeImage(const std::string& path, const uint8_t* rgb, int w, int h, bool png, int quality, std::string& err) {
  if (w <= 0 || h <= 0 || (uint64_t)w * 3 > (uint64_t)INT_MAX) { err = "image size out of range"; return false; }
  std::string part = path + ".part";
  FILE* f = ufopen(part, "wb");
  if (!f) { err = "cannot create file"; return false; }
  int ok;
  if (png) {
    stbi_write_png_compression_level = 6;
    ok = stbi_write_png_to_func(writeToFile, f, w, h, 3, rgb, w * 3);
  } else {
    ok = stbi_write_jpg_to_func(writeToFile, f, w, h, 3, rgb, quality);
  }
  bool closed = !ferror(f);  // the write callback cannot report a full disk itself
  closed = (fclose(f) == 0) && closed;
  return finishPart(part, path, ok && closed, err);
}

static void appendToVector(void* ctx, void* data, int size) {
  auto* v = (std::vector<uint8_t>*)ctx;
  v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

bool encodeJpegMem(const uint8_t* rgb, int w, int h, int quality, std::vector<uint8_t>& out) {
  out.clear();
  return stbi_write_jpg_to_func(appendToVector, &out, w, h, 3, rgb, quality) != 0;
}

bool decodeRgbMem(const uint8_t* data, size_t size, std::vector<uint8_t>& rgb, int& w, int& h) {
  std::string err;
  if (!checkImage(data, size, kMaxSmallPixels, err)) return false;
  int n = 0;
  stbi_uc* p = stbi_load_from_memory(data, (int)size, &w, &h, &n, 3);
  if (!p) return false;
  bool ok = true;
  try {
    rgb.assign(p, p + (size_t)w * h * 3);
  } catch (const std::bad_alloc&) {
    ok = false;
  }
  stbi_image_free(p);
  return ok;
}

bool writePngRgba(const std::string& path, const uint8_t* rgba, int w, int h, std::string& err) {
  if (w <= 0 || h <= 0 || (uint64_t)w * 4 > (uint64_t)INT_MAX) { err = "image size out of range"; return false; }
  std::string part = path + ".part";
  FILE* f = ufopen(part, "wb");
  if (!f) { err = "cannot create file"; return false; }
  stbi_write_png_compression_level = 6;
  int ok = stbi_write_png_to_func(writeToFile, f, w, h, 4, rgba, w * 4);
  bool closed = !ferror(f);  // the write callback cannot report a full disk itself
  closed = (fclose(f) == 0) && closed;
  return finishPart(part, path, ok && closed, err);
}

bool loadRgbaFile(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
  unsigned long long fsize = 0;
  long long mtime = 0;
  if (!fileInfo(path, fsize, mtime) || fsize > (256ull << 20)) return false;  // AI caches: a few MB
  std::vector<unsigned char> file;
  if (!readFileBytes(path, file)) return false;
  std::string err;
  if (!checkImage(file.data(), file.size(), 128ull << 20, err)) return false;  // magic-eraser patches can be large
  int n = 0;
  stbi_uc* p = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
  if (!p) return false;
  bool ok = true;
  try {
    rgba.assign(p, p + (size_t)w * h * 4);
  } catch (const std::bad_alloc&) {
    ok = false;
  }
  stbi_image_free(p);
  return ok;
}
