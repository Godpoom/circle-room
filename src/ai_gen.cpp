#include "ai_gen.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "ai/wic_decode.h"
#include "image_io.h"
#include "layer_content.h"
#include "layers.h"  // loadLayerPixels
#include "platform_win.h"
#include "updater.h"  // sha256Hex

namespace {

GenHook g_hook = nullptr;
const int kDecodeMax = 4096;  // photo pixels the patch is made at (long edge)

bool decodeResult(const orouter::Result& r, const std::string& tmpDir, ai::SmallImage& img, std::string& err) {
  if (decodeRgbMem(r.bytes.data(), r.bytes.size(), img.rgb, img.w, img.h) && img.w > 0) return true;
  // WebP and friends: Windows Imaging Component, from a temporary file.
  std::string ext = r.mime.find("webp") != std::string::npos ? ".webp" : r.mime.find("jpeg") != std::string::npos ? ".jpg" : ".png";
  std::string tmp = tmpDir + "\\gen_" + std::to_string(GetCurrentThreadId()) + ext;
  FILE* f = ufopen(tmp, "wb");
  if (!f) {
    err = "cannot write a temporary file";
    return false;
  }
  bool wrote = fwrite(r.bytes.data(), 1, r.bytes.size(), f) == r.bytes.size();
  fclose(f);
  bool ok = wrote && ai::decodeScaled(tmp, 8192, img, nullptr, err);
  deleteFileU(tmp);
  if (!ok && err.empty()) err = "cannot read the image the AI returned (" + r.mime + ")";
  return ok;
}

bool encodeJpeg(const ai::SmallImage& im, std::vector<uint8_t>& out) { return encodeJpegMem(im.rgb.data(), im.w, im.h, 92, out); }

std::string pickResolution(const orouter::Model& m, bool hiRes) {
  if (m.resolutions.empty()) return "";
  const char* want = hiRes ? "2K" : "1K";  // the size the price shown was for
  for (const std::string& r : m.resolutions)
    if (r == want) return r;
  for (const std::string& r : m.resolutions)
    if (r == "1K") return r;
  return "";
}

std::string pickFormat(const orouter::Model& m) {
  for (const std::string& f : m.formats)
    if (f == "png") return f;
  for (const std::string& f : m.formats)
    if (f == "jpeg") return f;
  return "";
}

bool writeLayerPng(const std::string& dir, const std::vector<uint8_t>& rgba, int w, int h, std::string& file, std::string& err) {
  if (!isDirectory(dir)) {
    if (!ensureDir(dir)) {
      err = "cannot create " + dir;
      return false;
    }
    setHiddenU(dir);
  }
  file = dir + "\\ai_" + sha256Hex(rgba.data(), rgba.size()).substr(0, 24) + ".png";
  if (fileExists(file)) return true;
  return writePngRgba(file, rgba.data(), w, h, err);
}

// Asks the model and returns its picture resampled to outW x outH.
bool ask(const GenInput& in, const std::string& prompt, const std::vector<orouter::Input>& imgs, float ratio, int outW, int outH,
         const std::string& tmpDir, ai::SmallImage& res, GenOutput& out) {
  if (!g_hook && orouter::cancelled(orouter::kGenerate)) {  // cancelled while the photo was being prepared
    out.err = "cancelled";
    return false;
  }
  orouter::Request rq;
  rq.model = in.model.id;
  rq.prompt = prompt;
  rq.aspect = orouter::nearestAspect(in.model, ratio);
  rq.resolution = pickResolution(in.model, in.hiRes);
  rq.format = pickFormat(in.model);
  rq.images = imgs;
  orouter::Result r;
  if (in.stage) *in.stage = GEN_STAGE_WAIT;
  if (!(g_hook ? g_hook(rq, r, out.err) : orouter::generate(in.key, rq, r, out.err))) return false;
  if (r.cost >= 0) out.cost = std::max(0.0, out.cost) + r.cost;
  if (in.stage) *in.stage = GEN_STAGE_PLACE;
  ai::SmallImage got;
  if (!decodeResult(r, tmpDir, got, out.err)) return false;
  if (outW <= 0 || outH <= 0) {
    res = std::move(got);
    return true;
  }
  ai::resample(got, 0, 0, (float)got.w, (float)got.h, outW, outH, res);
  return true;
}

}  // namespace

// ---------------------------------------------------------------- pieces
void setGenHook(GenHook h) { g_hook = h; }

void rasterMask(const std::vector<HealStroke>& strokes, int W, int H, std::vector<float>& m) {
  m.assign((size_t)W * H, 0.f);
  auto dab = [&](float x, float y, float r) {
    int x0 = std::max(0, (int)std::floor(x - r - 1)), x1 = std::min(W - 1, (int)std::ceil(x + r + 1));
    int y0 = std::max(0, (int)std::floor(y - r - 1)), y1 = std::min(H - 1, (int)std::ceil(y + r + 1));
    for (int yy = y0; yy <= y1; yy++)
      for (int xx = x0; xx <= x1; xx++) {
        float d = std::hypot(xx + 0.5f - x, yy + 0.5f - y);
        float v = std::clamp(r + 0.5f - d, 0.f, 1.f);
        float& o = m[(size_t)yy * W + xx];
        o = std::max(o, v);
      }
  };
  for (const HealStroke& st : strokes) {
    float r = std::max(1.f, st.radius * W);
    for (size_t k = 0; k + 1 < st.pts.size(); k += 2) {
      float x = st.pts[k] * W, y = st.pts[k + 1] * H;
      if (k >= 2) {
        float px = st.pts[k - 2] * W, py = st.pts[k - 1] * H;
        float len = std::hypot(x - px, y - py);
        int n = std::min(100000, (int)(len / std::max(1.f, r * 0.3f)));
        for (int i = 1; i < n; i++) dab(px + (x - px) * i / n, py + (y - py) * i / n, r);
      }
      dab(x, y, r);
    }
  }
}

void boxBlur(std::vector<float>& m, int W, int H, int r) {
  if (r <= 0 || W <= 0 || H <= 0) return;
  std::vector<float> tmp(m.size());
  auto pass = [&](const std::vector<float>& src, std::vector<float>& dst, int len, int count, size_t step, size_t stride) {
    std::vector<double> pre(len + 1);
    for (int c = 0; c < count; c++) {
      size_t base = (size_t)c * stride;
      pre[0] = 0;
      for (int i = 0; i < len; i++) pre[i + 1] = pre[i] + src[base + (size_t)i * step];
      for (int i = 0; i < len; i++) {
        int a = std::max(0, i - r), b = std::min(len - 1, i + r);
        dst[base + (size_t)i * step] = (float)((pre[b + 1] - pre[a]) / (b - a + 1));
      }
    }
  };
  pass(m, tmp, W, H, 1, (size_t)W);  // rows
  pass(tmp, m, H, W, (size_t)W, 1);  // columns
}

void contextRect(int bx0, int by0, int bx1, int by1, int W, int H, float R, int& x, int& y, int& w, int& h) {
  float bw = (float)(bx1 - bx0), bh = (float)(by1 - by0);
  float pad = std::max(48.f, 0.4f * std::max(bw, bh));
  float cw = std::min((float)W, bw + 2 * pad), ch = std::min((float)H, bh + 2 * pad);
  if (R > 0) {  // grow the short side to the model's aspect, then fit inside the photo
    if (cw / ch < R) cw = ch * R;
    else ch = cw / R;
    if (cw > W) {
      cw = (float)W;
      ch = std::min((float)H, cw / R);
    }
    if (ch > H) {
      ch = (float)H;
      cw = std::min((float)W, ch * R);
    }
  }
  w = std::max(1, (int)std::lround(cw));
  h = std::max(1, (int)std::lround(ch));
  float mx = (bx0 + bx1) * 0.5f, my = (by0 + by1) * 0.5f;
  x = std::clamp((int)std::lround(mx - w * 0.5f), 0, std::max(0, W - w));
  y = std::clamp((int)std::lround(my - h * 0.5f), 0, std::max(0, H - h));
}

std::string genPrompt(int task, const std::string& user, bool hasMask) {
  std::string u = user;
  while (!u.empty() && (u.back() == ' ' || u.back() == '\n')) u.pop_back();
  std::string what;
  switch (task) {
    case GEN_REMOVE:
    case GEN_MOVE:
      what = "remove what is there and fill in the background that would naturally be behind it";
      if (!u.empty()) what += " (" + u + ")";
      break;
    case GEN_HIGHLIGHT:
      what = "the area is overexposed and blown out to white; rebuild it with natural detail, texture and colour that match the "
             "surroundings (for example sky and clouds, skin, fabric or a wall), still bright but no longer pure white";
      if (!u.empty()) what += ". " + u;
      break;
    case GEN_NEW: return u;
    default: what = u.empty() ? "improve this area naturally" : u;
  }
  if (!hasMask)
    return "Edit this photograph: " + what +
           ". Keep the same framing, composition and camera angle, and return the whole photo as a realistic photograph.";
  return "You are retouching a photograph. The first image is the photo. The second image is a mask: change only the white "
         "area of the mask. In that area: " +
         what +
         ". Keep everything outside the white area exactly as it is, with the same framing, perspective, lighting, colour, grain "
         "and sharpness, so the change blends in seamlessly. Return the whole photo at the same framing, without the mask.";
}

std::string picturesAiDir() { return picturesSubDir("Circle Room AI"); }

std::string picturesSubDir(const char* name) {
  PWSTR p = nullptr;
  std::string dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)) && p) dir = wideToUtf8(p);
  if (p) CoTaskMemFree(p);
  if (dir.empty()) return "";
  dir += std::string("\\") + name;
  ensureDir(dir);
  return dir;
}

// ---------------------------------------------------------------- the job
bool runGen(const GenInput& in, GenOutput& out) {
  out = GenOutput();
  if (in.stage) *in.stage = GEN_STAGE_PREPARE;
  try {
    if (in.task == GEN_NEW) {
      std::string dir = picturesAiDir();
      if (dir.empty()) {
        out.err = "no Pictures folder";
        return false;
      }
      ai::SmallImage res;
      if (!ask(in, genPrompt(GEN_NEW, in.prompt, false), {}, in.aspect, 0, 0, dir, res, out)) return false;
      char name[64];
      time_t t = time(nullptr);
      tm lt;
      localtime_s(&lt, &t);
      strftime(name, sizeof(name), "AI_%Y%m%d_%H%M%S.png", &lt);
      out.newImage = dir + "\\" + name;
      return writeImage(out.newImage, res.rgb.data(), res.w, res.h, true, 95, out.err);
    }

    ai::SmallImage img;
    // The same pixels the editor and the magic eraser start from (for RAW: the linear decode, not
    // the camera's JPEG preview, whose tones differ): the patch then matches the photo under it.
    if (!loadLayerPixels(in.photo, kDecodeMax, img.rgb, img.w, img.h, out.err)) return false;
    const int W = img.w, H = img.h;
    const std::string dir = layerContentDir(in.photo);
    std::vector<float> m;
    bool hasMask = false;
    if (!in.mask.empty()) {
      rasterMask(in.mask, W, H, m);
      for (float v : m) hasMask |= v > 0.5f;
    } else if (in.task == GEN_HIGHLIGHT) {  // no painting: the blown-out areas themselves
      m.assign((size_t)W * H, 0.f);
      size_t n = 0;
      for (size_t i = 0; i < m.size(); i++)
        if (img.rgb[i * 3] >= 250 && img.rgb[i * 3 + 1] >= 250 && img.rgb[i * 3 + 2] >= 250) {
          m[i] = 1.f;
          n++;
        }
      if (n < m.size() / 2000) {
        out.err = "no blown-out area found; paint over the area instead";
        return false;
      }
      int g = std::max(2, std::min(W, H) / 200);  // grow a little past the clip edge
      boxBlur(m, W, H, g);
      for (float& v : m) v = v > 0.02f ? 1.f : 0.f;
      hasMask = true;
    }
    if (!hasMask && (in.task == GEN_REMOVE || in.task == GEN_MOVE)) {
      out.err = "paint over what to remove first";
      return false;
    }
    if (!in.model.edits) {
      out.err = "this model cannot edit photos; pick one marked 'edits photos'";
      return false;
    }

    if (!hasMask) {  // whole photo, as one layer on top
      ai::SmallImage send;
      ai::fitLongEdge(img, 1536, send);
      std::vector<orouter::Input> imgs(1);
      imgs[0].mime = "image/jpeg";
      encodeJpeg(send, imgs[0].bytes);
      int ow = W, oh = H;
      if (std::max(ow, oh) > 2048) {
        float k = 2048.f / std::max(ow, oh);
        ow = std::max(1, (int)(ow * k));
        oh = std::max(1, (int)(oh * k));
      }
      ai::SmallImage res;
      if (!ask(in, genPrompt(in.task, in.prompt, false), imgs, (float)W / H, ow, oh, dir, res, out)) return false;
      std::vector<uint8_t> rgba((size_t)ow * oh * 4, 255);
      for (size_t i = 0; i < (size_t)ow * oh; i++) std::memcpy(&rgba[i * 4], &res.rgb[i * 3], 3);
      GenLayer L;
      if (!writeLayerPng(dir, rgba, ow, oh, L.file, out.err)) return false;
      out.layers.push_back(L);
      return true;
    }

    // Bounding box of the painted area, then a crop with context at the model's aspect.
    int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (m[(size_t)y * W + x] > 0.5f) {
          bx0 = std::min(bx0, x);
          by0 = std::min(by0, y);
          bx1 = std::max(bx1, x + 1);
          by1 = std::max(by1, y + 1);
        }
    float R = 0;
    {
      float bw = (float)(bx1 - bx0), bh = (float)(by1 - by0), pad = std::max(48.f, 0.4f * std::max(bw, bh));
      orouter::nearestAspect(in.model, (bw + 2 * pad) / (bh + 2 * pad), &R);
      if (in.model.aspects.empty()) R = 0;
    }
    int cx, cy, cw, ch;
    contextRect(bx0, by0, bx1, by1, W, H, R, cx, cy, cw, ch);
    // What the model sees: the crop (at least 768 px on the long edge) and the mask.
    float k = std::clamp(1024.f / std::max(cw, ch), 768.f / std::max(cw, ch), 1536.f / std::max(cw, ch));
    int sw = std::max(16, (int)std::lround(cw * k)), sh = std::max(16, (int)std::lround(ch * k));
    ai::SmallImage crop, cropMask;
    ai::resample(img, (float)cx, (float)cy, (float)(cx + cw), (float)(cy + ch), sw, sh, crop);
    std::vector<orouter::Input> imgs(2);
    {
      ai::SmallImage mc;  // mask crop as a grey image
      mc.w = cw;
      mc.h = ch;
      mc.rgb.resize((size_t)cw * ch * 3);
      for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
          uint8_t v = m[(size_t)(cy + y) * W + cx + x] > 0.5f ? 255 : 0;
          std::memset(&mc.rgb[((size_t)y * cw + x) * 3], v, 3);
        }
      ai::resample(mc, 0, 0, (float)cw, (float)ch, sw, sh, cropMask);
      for (uint8_t& v : cropMask.rgb) v = v > 127 ? 255 : 0;
    }
    imgs[0].mime = imgs[1].mime = "image/jpeg";
    encodeJpeg(crop, imgs[0].bytes);
    encodeJpeg(cropMask, imgs[1].bytes);
    std::vector<uint8_t>().swap(crop.rgb);

    int genTask = in.task == GEN_MOVE ? GEN_REMOVE : in.task;
    ai::SmallImage res;
    if (!ask(in, genPrompt(genTask, in.prompt, true), imgs, (float)cw / ch, cw, ch, dir, res, out)) return false;

    // Soft alpha: the painted area fully covered, fading out over f pixels.
    int f = std::max(3, (int)std::lround(0.015f * std::max(cw, ch)));
    std::vector<float> a((size_t)cw * ch);
    for (int y = 0; y < ch; y++)
      for (int x = 0; x < cw; x++) a[(size_t)y * cw + x] = m[(size_t)(cy + y) * W + cx + x];
    std::vector<float> painted = a;
    boxBlur(a, cw, ch, f / 2);
    for (float& v : a) v = v > 0.02f ? 1.f : 0.f;  // grow by ~f/2
    boxBlur(a, cw, ch, f / 2);
    for (size_t i = 0; i < a.size(); i++) a[i] = std::max(a[i], painted[i]);

    // Tone match: the model may shift colours a little; line them up on a ring around the area.
    {
      std::vector<float> ring = painted;
      boxBlur(ring, cw, ch, f * 3);
      double d[3] = {0, 0, 0};
      size_t n = 0;
      for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
          size_t i = (size_t)y * cw + x;
          if (ring[i] < 0.01f || a[i] > 0.01f) continue;
          const uint8_t* o = &img.rgb[((size_t)(cy + y) * W + cx + x) * 3];
          for (int c = 0; c < 3; c++) d[c] += (double)o[c] - res.rgb[i * 3 + c];
          n++;
        }
      if (n > 50)
        for (size_t i = 0; i < a.size(); i++)
          for (int c = 0; c < 3; c++) {
            float v = res.rgb[i * 3 + c] + (float)(d[c] / n);
            res.rgb[i * 3 + c] = (uint8_t)std::clamp(std::lround(v), 0l, 255l);
          }
    }

    // Layers are trimmed to where they have alpha.
    auto emit = [&](const std::vector<float>& al, const std::vector<uint8_t>& rgb, int srcStride, int ox, int oy) -> bool {
      int x0 = cw, y0 = ch, x1 = -1, y1 = -1;
      for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
          if (al[(size_t)y * cw + x] > 0.004f) {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
          }
      if (x1 <= x0 || y1 <= y0) {
        out.err = "nothing to place";
        return false;
      }
      int lw = x1 - x0, lh = y1 - y0;
      std::vector<uint8_t> rgba((size_t)lw * lh * 4);
      for (int y = 0; y < lh; y++)
        for (int x = 0; x < lw; x++) {
          size_t li = (size_t)y * lw + x, aIdx = (size_t)(y + y0) * cw + x + x0;
          size_t si = (size_t)(y + y0 + oy) * srcStride + x + x0 + ox;
          std::memcpy(&rgba[li * 4], &rgb[si * 3], 3);
          rgba[li * 4 + 3] = (uint8_t)std::lround(std::clamp(al[aIdx], 0.f, 1.f) * 255.f);
        }
      GenLayer L;
      if (!writeLayerPng(dir, rgba, lw, lh, L.file, out.err)) return false;
      L.cx = (cx + x0 + lw * 0.5f) / W;
      L.cy = (cy + y0 + lh * 0.5f) / H;
      L.scale = (float)lw / W;
      out.layers.push_back(L);
      return true;
    };
    if (!emit(a, res.rgb, cw, 0, 0)) return false;
    if (in.task == GEN_MOVE) {  // the thing itself, cut from the original pixels, on top of the fill
      std::vector<float> pa = painted;
      boxBlur(pa, cw, ch, std::max(1, f / 3));
      if (!emit(pa, img.rgb, W, cx, cy)) return false;
    }
    return true;
  } catch (const std::bad_alloc&) {
    out.err = "out of memory";
    return false;
  }
}
