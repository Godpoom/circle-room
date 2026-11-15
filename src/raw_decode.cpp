#include "raw_decode.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#include "libraw/libraw.h"
#include "platform_win.h"

namespace {

const char* const kRawExt[] = {"3fr", "arw", "cr2", "cr3", "crw", "dcr", "dng", "erf", "fff", "iiq", "kdc", "mef", "mos",
                               "mrw", "nef", "nrw", "orf", "pef", "raf", "rw2", "rwl", "sr2", "srf", "srw", "x3f", nullptr};

// Same ceilings as the other decoders: refuse before a large buffer is ever allocated.
const uint64_t kMaxPixels = 500ull * 1000 * 1000;
const unsigned long long kMaxFileBytes = 1024ull * 1024 * 1024;  // the largest medium-format RAWs are ~400 MB

// LibRaw's flip (0, 3 = 180, 5 = 90 CCW, 6 = 90 CW) as an EXIF orientation code.
int exifFromFlip(int flip) {
  switch (flip) {
    case 3: return 3;
    case 5: return 8;
    case 6: return 6;
    default: return 1;
  }
}

bool readFile(const std::string& path, std::vector<unsigned char>& file, std::string& err) {
  unsigned long long size = 0;
  long long mtime = 0;
  if (!fileInfo(path, size, mtime)) { err = "cannot read file"; return false; }
  if (size == 0 || size > kMaxFileBytes) { err = "file too large"; return false; }
  if (!readFileBytes(path, file)) { err = "cannot read file"; return false; }
  return true;
}

// Opens the file and checks its claimed size before anything is unpacked.
bool openChecked(LibRaw& lr, const std::vector<unsigned char>& file, std::string& err) {
  int rc = lr.open_buffer(file.data(), file.size());
  if (rc != LIBRAW_SUCCESS) {
    err = std::string("not a supported RAW file (") + libraw_strerror(rc) + ")";
    return false;
  }
  const libraw_image_sizes_t& s = lr.imgdata.sizes;
  if (s.width == 0 || s.height == 0 || s.raw_width == 0 || s.raw_height == 0 ||
      (uint64_t)s.width * s.height > kMaxPixels || (uint64_t)s.raw_width * s.raw_height > kMaxPixels) {
    err = "image dimensions out of range";
    return false;
  }
  return true;
}

template <class T>
void halve(std::vector<T>& px, int& w, int& h) {
  int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
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

struct MemImage {
  libraw_processed_image_t* p = nullptr;
  ~MemImage() {
    if (p) LibRaw::dcraw_clear_mem(p);
  }
};

}  // namespace

bool isRawFile(const std::string& path) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || path.find_first_of("\\/", dot) != std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = (char)tolower((unsigned char)c);
  for (const char* const* e = kRawExt; *e; e++)
    if (ext == *e) return true;
  return false;
}

const char* const* rawExtensions() { return kRawExt; }

bool loadRawImage(const std::string& path, int maxDim, ImageData& out, std::string& err) {
  out = ImageData();
  std::vector<unsigned char> file;
  if (!readFile(path, file, err)) return false;
  std::unique_ptr<LibRaw> lr(new LibRaw(0));
  libraw_output_params_t& p = lr->imgdata.params;
  p.output_bps = 16;
  p.gamm[0] = 1.0;  // linear: the renderer works in linear light
  p.gamm[1] = 1.0;
  p.no_auto_bright = 1;  // keep the exposure the photographer chose; the sliders take it from there
  p.use_camera_wb = 1;
  p.output_color = 1;  // sRGB primaries
  p.user_flip = 0;     // the renderer applies the orientation, as it does for JPEGs
  p.highlight = 0;     // clip: blown channels stay neutral instead of turning magenta
  if (!openChecked(*lr, file, err)) return false;
  maxDim = std::max(1, maxDim);
  const libraw_image_sizes_t& s = lr->imgdata.sizes;
  if (std::max(s.width, s.height) / 2 >= maxDim) p.half_size = 1;  // bigger than the GPU can hold anyway
  int rc = lr->unpack();
  if (rc == LIBRAW_SUCCESS) rc = lr->dcraw_process();
  if (rc != LIBRAW_SUCCESS) {
    err = std::string("cannot decode this RAW file (") + libraw_strerror(rc) + ")";
    return false;
  }
  std::vector<unsigned char>().swap(file);
  MemImage img;
  img.p = lr->dcraw_make_mem_image(&rc);
  if (!img.p || rc != LIBRAW_SUCCESS || img.p->type != LIBRAW_IMAGE_BITMAP || img.p->colors != 3 || img.p->bits != 16 ||
      img.p->width == 0 || img.p->height == 0 ||
      img.p->data_size < (unsigned)img.p->width * img.p->height * 3 * 2) {
    err = "cannot decode this RAW file";
    return false;
  }
  int w = img.p->width, h = img.p->height;
  int flip = s.flip;
  out.px16.resize((size_t)w * h * 3);
  std::memcpy(out.px16.data(), img.p->data, out.px16.size() * sizeof(uint16_t));
  lr.reset();  // frees LibRaw's own buffers before the downscale below
  while (w > maxDim || h > maxDim) halve(out.px16, w, h);
  out.w = w;
  out.h = h;
  out.is16 = true;
  out.orientation = exifFromFlip(flip);
  return true;
}

bool loadRawPreview(const std::string& path, int minEdge, RawPreview& out, std::string& err) {
  out = RawPreview();
  std::vector<unsigned char> file;
  if (!readFile(path, file, err)) return false;
  std::unique_ptr<LibRaw> lr(new LibRaw(0));
  if (!openChecked(*lr, file, err)) return false;
  const libraw_image_sizes_t& s = lr->imgdata.sizes;
  out.orientation = exifFromFlip(s.flip);
  out.fullW = s.width;
  out.fullH = s.height;
  out.shotTime = (int64_t)lr->imgdata.other.timestamp;
  double aspect = (double)s.width / s.height;

  // 1. The camera's own preview: no demosaic, a few milliseconds.
  if (lr->unpack_thumb() == LIBRAW_SUCCESS) {
    const libraw_thumbnail_t& t = lr->imgdata.thumbnail;
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    bool ok = false;
    if (t.tformat == LIBRAW_THUMBNAIL_JPEG && t.thumb && t.tlength > 0) {
      ok = decodeRgbMem((const uint8_t*)t.thumb, t.tlength, rgb, w, h);
    } else if (t.tformat == LIBRAW_THUMBNAIL_BITMAP && t.thumb && t.tcolors == 3 && t.twidth > 0 && t.theight > 0 &&
               (size_t)t.tlength >= (size_t)t.twidth * t.theight * 3) {
      w = t.twidth;
      h = t.theight;
      rgb.assign((const uint8_t*)t.thumb, (const uint8_t*)t.thumb + (size_t)w * h * 3);
      ok = true;
    }
    // Some previews are stored already rotated or cropped to another shape: use only one that
    // frames the same picture as the RAW, so AI masks line up with what the editor shows.
    if (ok && w > 0 && h > 0 && std::max(w, h) >= minEdge && std::fabs((double)w / h - aspect) / aspect < 0.02) {
      out.w = w;
      out.h = h;
      out.rgb.swap(rgb);
      return true;
    }
  }

  // 2. A half-size decode, sRGB 8-bit (about a second for a 24 MP file).
  libraw_output_params_t& p = lr->imgdata.params;
  p.half_size = 1;
  p.output_bps = 8;
  p.use_camera_wb = 1;
  p.output_color = 1;
  p.user_flip = 0;
  p.no_auto_bright = 1;
  p.highlight = 0;
  int rc = lr->unpack();
  if (rc == LIBRAW_SUCCESS) rc = lr->dcraw_process();
  if (rc != LIBRAW_SUCCESS) {
    err = std::string("cannot decode this RAW file (") + libraw_strerror(rc) + ")";
    return false;
  }
  std::vector<unsigned char>().swap(file);
  MemImage img;
  img.p = lr->dcraw_make_mem_image(&rc);
  if (!img.p || rc != LIBRAW_SUCCESS || img.p->type != LIBRAW_IMAGE_BITMAP || img.p->colors != 3 || img.p->bits != 8 ||
      img.p->width == 0 || img.p->height == 0 || img.p->data_size < (unsigned)img.p->width * img.p->height * 3) {
    err = "cannot decode this RAW file";
    return false;
  }
  out.w = img.p->width;
  out.h = img.p->height;
  out.rgb.assign(img.p->data, img.p->data + (size_t)out.w * out.h * 3);
  return true;
}
