// CircleRoom.exe --selftest [work_dir]: regression tests for damaged or crafted input files
// (analysis cache, thumbnails, images, .pedit sidecars, brush strokes, layer paths, writes).
// Every case must be rejected or repaired without a crash or a large allocation.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ai/cache.h"
#include "ai/wic_decode.h"
#include "image_io.h"
#include "layers.h"
#include "params.h"
#include "platform_win.h"
#include "selftest.h"

namespace {

int g_fail = 0, g_pass = 0;

void check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  (ok ? g_pass : g_fail)++;
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}
void putBE32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 3; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i)));
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& b) {
  deleteFileU(path);  // the cache files are hidden, and hidden files cannot be reopened for writing
  FILE* f = ufopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
  return (fclose(f) == 0) && ok;
}

// PNG with only a signature and an IHDR claiming w x h (no image data).
std::vector<uint8_t> pngHeader(uint32_t w, uint32_t h) {
  std::vector<uint8_t> b = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  putBE32(b, 13);
  for (char c : std::string("IHDR")) b.push_back((uint8_t)c);
  putBE32(b, w);
  putBE32(b, h);
  b.insert(b.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  putBE32(b, 0);                       // CRC (not verified by the decoders' header readers)
  return b;
}

std::vector<uint8_t> cacheHeader(uint32_t n) {
  std::vector<uint8_t> b = {'P', 'E', 'A', 'I'};
  put32(b, 2);
  put32(b, (uint32_t)sizeof(ai::Features));
  put32(b, n);
  return b;
}

void testCache(const std::string& dir) {
  printf("analysis cache\n");
  std::string folder = dir + "\\cache";
  ensureDir(folder);
  std::vector<ai::PhotoInfo> out;
  // 1. claims 4 billion entries in a 16-byte file
  writeBytes(folder + "\\.photoeditor_ai", cacheHeader(0xFFFFFFFFu));
  check(!ai::loadCache(folder, out) && out.empty(), "huge entry count is rejected before reserving memory");
  // 2. a valid round trip still works, and a NaN entry is dropped, not loaded
  std::vector<ai::PhotoInfo> items(3);
  for (int i = 0; i < 3; i++) {
    items[i].path = folder + "\\p" + std::to_string(i) + ".jpg";
    items[i].w = 100;
    items[i].h = 80;
    items[i].hasAi = true;
    items[i].aesthetic = 5.f + i;
  }
  items[1].aesthetic = NAN;
  check(ai::saveCache(folder, items), "cache saves");
  bool loaded = ai::loadCache(folder, out);
  check(loaded && out.size() == 2 && out[0].aesthetic == 5.f && out[1].aesthetic == 7.f,
        "valid entries load, the NaN entry is skipped");
  // 3. truncated file
  std::vector<unsigned char> full;
  readFileBytes(folder + "\\.photoeditor_ai", full);
  full.resize(full.size() - 100);
  writeBytes(folder + "\\.photoeditor_ai", std::vector<uint8_t>(full.begin(), full.end()));
  check(!ai::loadCache(folder, out) && out.empty(), "truncated cache is rejected");
  // 4. thumbnails: slices outside the file or absurdly large are refused
  std::vector<uint8_t> thumbs(1000, 7);
  writeBytes(folder + "\\.photoeditor_thumbs", thumbs);
  std::vector<uint8_t> t;
  check(ai::readThumb(folder, 100, 200, t) && t.size() == 200, "valid thumbnail slice reads");
  check(!ai::readThumb(folder, 900, 200, t) && t.empty(), "slice past the end is refused");
  check(!ai::readThumb(folder, 0, 0xFFFFFFF0u, t) && t.empty(), "4 GB length is refused without allocating");
  check(!ai::readThumb(folder, 0xFFFFFFFFFFFFull, 10, t), "offset far beyond the file is refused");
  // 5. clearing the cache removes only our files
  writeBytes(folder + "\\p0.jpg", thumbs);
  writeTextFile(folder + "\\p0.jpg.pedit", "exposure=1\n");
  ensureDir(folder + "\\.photoeditor_cache");
  writeBytes(folder + "\\.photoeditor_cache\\p0.jpg.heal-0123456789abcdef.png", thumbs);
  writeBytes(folder + "\\.photoeditor_cache\\keep.txt", thumbs);
  int n = ai::clearFolderCache(folder);
  check(n == 3 && fileExists(folder + "\\p0.jpg") && fileExists(folder + "\\p0.jpg.pedit") &&
            fileExists(folder + "\\.photoeditor_cache\\keep.txt") && !fileExists(folder + "\\.photoeditor_ai"),
        "cache clear removes only the app's cache files");
}

void testImages(const std::string& dir) {
  printf("images\n");
  std::string err;
  ImageData img;
  std::vector<uint8_t> bomb = pngHeader(100000, 100000);
  std::string p = dir + "\\bomb.png";
  writeBytes(p, bomb);
  check(!loadImage(p, 8192, img, err) && img.px8.empty(), ("10-gigapixel PNG header is refused: " + err).c_str());
  ai::SmallImage si;
  err.clear();
  check(!ai::decodeScaled(p, 512, si, nullptr, err), ("WIC refuses it too: " + err).c_str());
  writeBytes(p, pngHeader(1, 200000));
  check(!loadImage(p, 8192, img, err), "200000-px side is refused");
  std::vector<uint8_t> rgb;
  int w = 0, h = 0;
  check(!decodeRgbMem(bomb.data(), bomb.size(), rgb, w, h), "thumbnail decoder refuses it");
  std::vector<uint8_t> junk = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x10, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I'};
  writeBytes(p, junk);
  check(!loadImage(p, 8192, img, err), "truncated JPEG with broken EXIF fails cleanly");
  // a valid tall, thin image still loads and is halved without going to 0 px wide
  std::vector<uint8_t> strip(1 * 9000 * 3, 128);
  std::string sp = dir + "\\strip.png";
  check(writeImage(sp, strip.data(), 1, 9000, true, 90, err), "1 x 9000 PNG writes");
  check(loadImage(sp, 2048, img, err) && img.w == 1 && img.h > 0 && img.h <= 2048, "1 x 9000 PNG loads and halves to 1 px wide");
  // a valid normal image is unaffected
  std::vector<uint8_t> px(640 * 480 * 3, 90);
  std::string np = dir + "\\normal.jpg";
  check(writeImage(np, px.data(), 640, 480, false, 90, err) && loadImage(np, 8192, img, err) && img.w == 640 && img.h == 480,
        "ordinary 640 x 480 JPEG round trip");
  check(ai::decodeScaled(np, 256, si, nullptr, err) && si.w == 256, "WIC scaled decode of a normal JPEG");
}

void testParams() {
  printf(".pedit values\n");
  EditParams p;
  deserializeParams("exposure=nan\ncontrast=inf\nsaturation=-1e30\ncropX=0.5\ncropW=1000\ncropY=-5\ncropH=nan\n"
                    "straighten=900\nsharpRadius=1e9\nlayer0Enabled=1\nlayer0Region=99999\nlayer1Enabled=1\n"
                    "layer1Region=1\nlayer1Feather=1e12\ngrade0Hue=-30\ncurve0=0,0;nan,0.5;1,1\nheals=999999\n"
                    "img0Enabled=1\nimg0Id=-4\nimg0Path=C:\\a.jpg\nimg0Scale=1e20\nimg0Strokes=99999999\n"
                    "img1Enabled=1\nimg1Id=1\nimg1Path=C:\\b.jpg\nimg2Enabled=1\nimg2Id=1\nimg2Path=C:\\c.jpg\n",
                    p, true);
  check(p.exposure == 0.f && p.contrast == 0.f && p.saturation == -200.f, "NaN / inf / huge tone values are fixed");
  check(p.cropW == 1.f && p.cropX == 0.f && p.cropY == 0.f && p.cropH == 1.f, "crop is forced inside the frame");
  check(p.straighten == 45.f && p.sharpRadius == 3.f, "straighten and sharpen radius are clamped");
  check(!p.layers[0].enabled && p.layers[1].enabled && p.layers[1].feather == 100.f, "bad region disabled, feather clamped");
  check(std::fabs(p.grade[0].hue - 330.f) < 1e-3f, "hue wraps into 0..360");
  check(p.curve[0].n == 2, "curve with a NaN point falls back to the identity curve");
  check(p.heals == kMaxHeals, "magic-eraser count is capped");
  bool idsOk = p.img[0].id > 0 && p.img[1].id > 0 && p.img[2].id > 0 && p.img[0].id != p.img[1].id &&
               p.img[1].id != p.img[2].id && p.img[0].id != p.img[2].id;
  check(idsOk, "photo-layer ids are positive and unique");
  check(p.img[0].scale == 50.f && p.img[0].strokes == 0, "layer scale clamped, strokes of a re-numbered layer dropped");
  std::string longPath(kLayerPathMax + 10, 'a');
  EditParams q;
  deserializeParams("img0Enabled=1\nimg0Id=1\nimg0Path=C:\\" + longPath + ".jpg\n", q, true);
  check(!q.img[0].enabled && q.img[0].path[0] == 0, "over-long layer path is dropped, not truncated");
  // A valid file from an older version (and AI styles at 150%) loads unchanged.
  EditParams a;
  a.exposure = 1.25f;
  a.contrast = 140.f;  // AI style at 150% strength
  a.temp = -35.f;
  a.cropX = 0.1f;
  a.cropY = 0.2f;
  a.cropW = 0.5f;
  a.cropH = 0.6f;
  a.straighten = 3.5f;
  a.grade[2].hue = 40.f;
  a.grade[2].sat = 22.f;
  a.curve[0].n = 3;
  a.curve[0].p[1] = {0.5f, 0.6f};
  a.curve[0].p[2] = {1.f, 1.f};
  a.layers[0].enabled = 1;
  a.layers[0].region = 1;
  a.layers[0].feather = 10.f;
  a.layers[0].exposure = -0.5f;
  EditParams b;
  deserializeParams(serializeParams(a, true), b, true);
  check(serializeParams(a, true) == serializeParams(b, true), "valid sidecar (incl. 150% AI style) round-trips unchanged");
  EditParams legacy;
  deserializeParams("exposure=0.4\ncontrast=25\nlayer0Enabled=1\nlayer0Region=0\nlayer0Exposure=0.3\n", legacy, true);
  check(legacy.exposure == 0.4f && legacy.contrast == 25.f && legacy.layers[0].enabled, "older sidecar without new keys reads");
}

void testStrokes() {
  printf("brush strokes\n");
  std::vector<float> pts = {0.1f, 0.2f, NAN, 0.5f, 1e30f, -1e30f, 0.3f, 0.4f};
  check(cleanStroke(pts, 0.05f, -1.f, 2.f, 2.f, kMaxStrokePoints) && pts.size() == 6 && pts[2] == 2.f && pts[3] == -1.f,
        "non-finite points dropped, far points clamped");
  std::vector<float> one = {0.5f, 0.5f};
  check(!cleanStroke(one, 1e9f, 0.f, 1.f, 0.5f, kMaxStrokePoints), "absurd radius is refused");
  check(!cleanStroke(one, NAN, 0.f, 1.f, 0.5f, kMaxStrokePoints), "NaN radius is refused");
  std::vector<float> many(200000 * 2, 0.5f);
  check(cleanStroke(many, 0.01f, 0.f, 1.f, 0.5f, kMaxStrokePoints) && many.size() == kMaxStrokePoints * 2,
        "point count is capped");
  std::vector<float> normal = {0.1f, 0.1f, 0.3f, 0.2f, 0.6f, 0.4f};
  std::vector<float> zigzag;
  for (int i = 0; i < 5000; i++) zigzag.insert(zigzag.end(), {-1.f, -1.f, 2.f, 2.f});
  check(strokeCost(normal, 0.03f, 0.25f) < kMaxStrokeCost, "an ordinary stroke is within the work budget");
  check(strokeCost(zigzag, 0.5f, 0.25f) > kMaxStrokeCost, "a crafted zig-zag stroke exceeds it (and is skipped)");
}

void testPaths(const std::string& dir) {
  printf("layer paths\n");
  std::string why, img = dir + "\\normal.jpg";
  check(layerPathAllowed(img, why), "local image file is allowed");
  check(!layerPathAllowed("\\\\attacker\\share\\x.jpg", why), "UNC share is refused");
  check(!layerPathAllowed("//attacker/share/x.jpg", why), "forward-slash UNC is refused");
  check(!layerPathAllowed("\\\\?\\C:\\x.jpg", why), "\\\\?\\ device path is refused");
  check(!layerPathAllowed("\\\\.\\PhysicalDrive0", why), "\\\\.\\ device is refused");
  check(!layerPathAllowed("x.jpg", why), "relative path is refused");
  check(!layerPathAllowed(img + ":stream", why), "alternate data stream is refused");
  check(!layerPathAllowed(dir + "\\notes.txt", why), "non-image extension is refused");
  check(!layerPathAllowed(dir + "\\missing.jpg", why), "missing file is refused");
}

void testWrites(const std::string& dir) {
  printf("writes\n");
  std::string f = dir + "\\side.pedit";
  check(writeTextFile(f, "exposure=1\n") && !fileExists(f + ".tmp"), "text write leaves no temp file");
  std::string t;
  check(readTextFile(f, t) && t == "exposure=1\n", "text write content");
  std::string err;
  std::vector<uint8_t> px(16 * 16 * 3, 1);
  std::string bad = dir + "\\no_such_dir\\x.jpg";
  check(!writeImage(bad, px.data(), 16, 16, false, 90, err) && !fileExists(bad + ".part"), "failed export leaves nothing behind");
  std::string jp = dir + "\\out.jpg";
  check(writeImage(jp, px.data(), 16, 16, false, 90, err) && fileExists(jp) && !fileExists(jp + ".part"), "export completes");
  std::string big(kMaxSidecarBytes + 16, 'x');
  std::string photo = dir + "\\big.jpg";
  writeBytes(photo + ".pedit", std::vector<uint8_t>(big.begin(), big.end()));
  check(!readSidecar(photo, t), "oversized sidecar is ignored");
}

}  // namespace

int runSelfTest(const std::vector<std::string>& args) {
  attachParentConsole();
  std::string dir = args.size() > 2 ? args[2] : dirOf(exeDir()) + "\\selftest_tmp";
  ensureDir(dir);
  printf("Circle Room self-test in %s\n", dir.c_str());
  testCache(dir);
  testImages(dir);
  testParams();
  testStrokes();
  testPaths(dir);
  testWrites(dir);
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
