#include "image_io.h"

#include <cmath>
#include <cstring>

#include "platform_win.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
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

template <class T>
static void halve(std::vector<T>& px, int& w, int& h) {
  int nw = w / 2, nh = h / 2;
  std::vector<T> out((size_t)nw * nh * 3);
  for (int y = 0; y < nh; y++)
    for (int x = 0; x < nw; x++)
      for (int c = 0; c < 3; c++) {
        size_t a = ((size_t)(2 * y) * w + 2 * x) * 3 + c, b = a + (size_t)w * 3;
        out[((size_t)y * nw + x) * 3 + c] = (T)((px[a] + px[a + 3] + px[b] + px[b + 3] + 2) / 4);
      }
  px.swap(out);
  w = nw;
  h = nh;
}

bool loadImage(const std::string& path, int maxDim, ImageData& out, std::string& err) {
  std::vector<unsigned char> file;
  if (!readFileBytes(path, file)) { err = "cannot read file"; return false; }
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

static void writeToFile(void* ctx, void* data, int size) { fwrite(data, 1, (size_t)size, (FILE*)ctx); }

bool writeImage(const std::string& path, const uint8_t* rgb, int w, int h, bool png, int quality, std::string& err) {
  FILE* f = ufopen(path, "wb");
  if (!f) { err = "cannot create file"; return false; }
  int ok;
  if (png) {
    stbi_write_png_compression_level = 6;
    ok = stbi_write_png_to_func(writeToFile, f, w, h, 3, rgb, w * 3);
  } else {
    ok = stbi_write_jpg_to_func(writeToFile, f, w, h, 3, rgb, quality);
  }
  bool closed = fclose(f) == 0;
  if (!ok || !closed) { err = "write failed"; return false; }
  return true;
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
  int n = 0;
  stbi_uc* p = stbi_load_from_memory(data, (int)size, &w, &h, &n, 3);
  if (!p) return false;
  rgb.assign(p, p + (size_t)w * h * 3);
  stbi_image_free(p);
  return true;
}

bool writePngRgba(const std::string& path, const uint8_t* rgba, int w, int h, std::string& err) {
  FILE* f = ufopen(path, "wb");
  if (!f) { err = "cannot create file"; return false; }
  stbi_write_png_compression_level = 6;
  int ok = stbi_write_png_to_func(writeToFile, f, w, h, 4, rgba, w * 4);
  bool closed = fclose(f) == 0;
  if (!ok || !closed) { err = "write failed"; return false; }
  return true;
}

bool loadRgbaFile(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
  std::vector<unsigned char> file;
  if (!readFileBytes(path, file)) return false;
  int n = 0;
  stbi_uc* p = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
  if (!p) return false;
  rgba.assign(p, p + (size_t)w * h * 4);
  stbi_image_free(p);
  return true;
}
