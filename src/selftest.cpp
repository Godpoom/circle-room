// CircleRoom.exe --selftest [work_dir]: regression tests for damaged or crafted input files
// (analysis cache, thumbnails, images, .pedit sidecars, brush strokes, layer paths, writes).
// Every case must be rejected or repaired without a crash or a large allocation.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <process.h>
#include <string>
#include <vector>

#include "agent_log.h"
#include "ai_gen.h"
#include "mcp_json.h"
#include "openrouter.h"
#include "font_maker.h"
#include "layer_content.h"
#include "raw_decode.h"
#include "updater.h"
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

// The MCP server's activity log, read back the way the app's AI card reads it.
void testAgentLog() {
  AgentWatch w;  // not started: poll() reads the folder directly
  std::vector<AgentEvent> fin;
  w.poll(0, fin);
  fin.clear();
  {
    AgentLog log;
    log.open("claude-code");
    log.call(1, "set_adjustments", "C:\\p\\a.jpg", "exposure=0.3");
    w.refresh();
    w.poll(1, fin);
    const AgentSession* s = nullptr;
    for (const AgentSession& x : w.sessions())
      if (x.alive && x.client == "Claude Code") s = &x;
    check(s && s->current() && s->current()->tool == "set_adjustments", "agent card sees the connected client and its running tool");
    log.done(1, "set_adjustments", true, "C:\\p\\a.jpg", {{"exposure", "0", "0.3"}}, "Applied.");
    w.refresh();
    w.poll(2, fin);
    check(fin.size() == 1 && fin[0].changes.size() == 1 && fin[0].changes[0].after == "0.3" && !fin[0].running,
          "finished step carries what changed");
  }  // destructor says bye
  w.refresh();
  w.poll(3, fin);
  bool gone = true;
  for (const AgentSession& x : w.sessions()) gone &= !x.alive;
  check(gone && !fileExists(agentLogDir() + "\\" + std::to_string(_getpid()) + ".log"),
        "disconnect is shown and the log file removed");
  w.poll(20, fin);
  check(w.sessions().empty(), "disconnected agent leaves the card after a few seconds");
  check(agentClientName("codex-mcp-client") == "Codex" && agentClientName("Visual Studio Code") == "VS Code",
        "MCP client names are shown by product name");
}

// A stored (uncompressed) zip, built by hand: enough to test the reader without a zip library.
std::vector<uint8_t> storedZip(const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<uint8_t> z, cd;
  auto p16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 255); v.push_back(x >> 8 & 255); };
  auto p32 = [&](std::vector<uint8_t>& v, uint32_t x) { p16(v, x & 0xFFFF); p16(v, x >> 16); };
  for (const auto& f : files) {
    uint32_t off = (uint32_t)z.size(), crc = crc32Of(f.second.data(), f.second.size()), n = (uint32_t)f.second.size();
    p32(z, 0x04034b50); p16(z, 20); p16(z, 0); p16(z, 0); p32(z, 0); p32(z, crc); p32(z, n); p32(z, n);
    p16(z, (uint32_t)f.first.size()); p16(z, 0);
    z.insert(z.end(), f.first.begin(), f.first.end());
    z.insert(z.end(), f.second.begin(), f.second.end());
    p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 20); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, crc); p32(cd, n);
    p32(cd, n); p16(cd, (uint32_t)f.first.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, off);
    cd.insert(cd.end(), f.first.begin(), f.first.end());
  }
  uint32_t cdOff = (uint32_t)z.size();
  z.insert(z.end(), cd.begin(), cd.end());
  p32(z, 0x06054b50); p16(z, 0); p16(z, 0); p16(z, (uint32_t)files.size()); p16(z, (uint32_t)files.size());
  p32(z, (uint32_t)cd.size()); p32(z, cdOff); p16(z, 0);
  return z;
}

// AI generate: the OpenRouter pieces offline, and the whole edit pipeline with a stand-in model
// that paints the white area of the mask green.
static bool fakeModel(const orouter::Request& rq, orouter::Result& out, std::string& err) {
  if (rq.images.size() != 2 || rq.aspect.empty() || rq.prompt.find("bird") == std::string::npos) {
    err = "fake model: unexpected request";
    return false;
  }
  std::vector<uint8_t> img, mk;
  int w = 0, h = 0, mw = 0, mh = 0;
  if (!decodeRgbMem(rq.images[0].bytes.data(), rq.images[0].bytes.size(), img, w, h) ||
      !decodeRgbMem(rq.images[1].bytes.data(), rq.images[1].bytes.size(), mk, mw, mh) || mw != w || mh != h) {
    err = "fake model: bad input images";
    return false;
  }
  for (size_t i = 0; i < (size_t)w * h; i++)
    if (mk[i * 3] > 127) {
      img[i * 3] = 20;
      img[i * 3 + 1] = 200;
      img[i * 3 + 2] = 30;
    }
  out.mime = "image/jpeg";
  out.cost = 0.01;
  return encodeJpegMem(img.data(), w, h, 95, out.bytes);
}

void testAiGen(const std::string& dir) {
  // base64, request and answer
  std::vector<uint8_t> raw = {0, 1, 2, 250, 251, 252, 7}, back;
  check(orouter::unbase64(orouter::base64(raw.data(), raw.size()), back) && back == raw, "base64 round trip");
  orouter::Request rq;
  rq.model = "a/b";
  rq.prompt = "นก \"bird\"";
  rq.aspect = "4:3";
  rq.images.resize(1);
  rq.images[0].mime = "image/jpeg";
  rq.images[0].bytes = raw;
  Json j;
  bool okJ = json::parse(orouter::buildRequestJson(rq), j);
  const Json* refs = j.get("input_references");
  check(okJ && j.str("model") == "a/b" && j.str("prompt") == rq.prompt && j.str("aspect_ratio") == "4:3" && refs && refs->isArr() &&
            refs->a.size() == 1 && refs->a[0].get("image_url") &&
            refs->a[0].get("image_url")->str("url").rfind("data:image/jpeg;base64,", 0) == 0,
        "OpenRouter request JSON has model, prompt, aspect and the image as a data URL");
  orouter::Result res;
  std::string err;
  std::string ans = "{\"data\":[{\"b64_json\":\"" + orouter::base64(raw.data(), raw.size()) +
                    "\",\"media_type\":\"image/png\"}],\"usage\":{\"cost\":0.04}}";
  check(orouter::parseResult(ans, res, err) && res.bytes == raw && res.mime == "image/png" && std::fabs(res.cost - 0.04) < 1e-9,
        "OpenRouter answer: image bytes, type and cost");
  check(!orouter::parseResult("{\"data\":[]}", res, err), "an answer without an image is an error");

  // model lists: capabilities + prices + free variants
  std::string imgs =
      "{\"data\":[{\"id\":\"x/edit\",\"name\":\"Edit Model\",\"architecture\":{\"input_modalities\":[\"text\",\"image\"]},"
      "\"supported_parameters\":{\"aspect_ratio\":{\"type\":\"enum\",\"values\":[\"1:1\",\"16:9\"]},\"input_references\":{}}},"
      "{\"id\":\"y/text\",\"name\":\"Text Model\",\"architecture\":{\"input_modalities\":[\"text\"]},\"supported_parameters\":{}}]}";
  std::string prices =
      "{\"data\":[{\"id\":\"x/edit\",\"name\":\"Edit Model\",\"pricing\":{\"image\":\"0.003\",\"prompt\":\"0\"}},"
      "{\"id\":\"y/text:free\",\"name\":\"Text Model (free)\",\"pricing\":{\"prompt\":\"0\"}},"
      "{\"id\":\"openrouter/auto\",\"pricing\":{}}]}";
  std::vector<orouter::Model> ms;
  bool okM = orouter::parseModels(imgs, prices, ms);
  bool order = okM && ms.size() == 3 && ms[0].id == "y/text:free" && ms[0].free && !ms[0].edits;
  const orouter::Model* e = nullptr;
  for (const orouter::Model& m : ms)
    if (m.id == "x/edit") e = &m;
  check(order && e && e->edits && !e->free && e->outCost < 0 /* the "image" key is the input price: the real one comes from the endpoints */ && e->aspects.size() == 2,
        "model list: free first, edit support and per-image price read");
  float ex = 0;
  check(e && orouter::nearestAspect(*e, 1.7f, &ex) == "16:9" && std::fabs(ex - 16.f / 9.f) < 1e-4f, "nearest aspect ratio");
  {  // prices per picture from the endpoints answer
    orouter::Model pm;
    std::string perImg =
        "{\"endpoints\":[{\"pricing\":[{\"billable\":\"output_image\",\"unit\":\"image\",\"cost_usd\":0.02},"
        "{\"billable\":\"output_image\",\"unit\":\"image\",\"cost_usd\":0.035,\"variant\":\"high_resolution\"},"
        "{\"billable\":\"input_image\",\"unit\":\"image\",\"cost_usd\":0.008}]}]}";
    pm.id = "selftest/per-image";
    pm.resolutions = {"1K", "2K"};
    bool p1 = orouter::parseEndpointPrice(perImg, pm) && std::fabs(pm.outCost - 0.02) < 1e-9 && std::fabs(pm.inCost - 0.008) < 1e-9 &&
              !pm.approx && orouter::priceText(pm, false, false) == "$0.020" && orouter::priceText(pm, true, false) == "$0.036" &&
              orouter::priceText(pm, false, true) == "$0.035";
    orouter::Model tm;  // the model's own tokens (Gemini style): ~1290 per picture
    tm.id = "selftest/native-tokens";
    std::string perTok =
        "{\"endpoints\":[{\"pricing\":[{\"billable\":\"output_image\",\"unit\":\"token\",\"cost_usd\":0.00006}]},"
        "{\"pricing\":[{\"billable\":\"output_image\",\"unit\":\"token\",\"cost_usd\":0.00003}]}]}";
    bool p2 = orouter::parseEndpointPrice(perTok, tm) && tm.approx && std::fabs(tm.outCost - 0.0387) < 1e-6 &&
              orouter::priceText(tm, false, false) == "~$0.039";
    orouter::Model nm;  // OpenRouter image tokens: 4175 per 1K picture, about 4x at 2K
    nm.id = "selftest/or-tokens";
    nm.normTok = true;
    nm.resolutions = {"1K", "2K"};
    bool p3 = orouter::parseEndpointPrice(
                  "{\"endpoints\":[{\"pricing\":[{\"billable\":\"output_image\",\"unit\":\"token\",\"cost_usd\":0.0000016}]}]}", nm) &&
              orouter::priceText(nm, false, false) == "~$0.007" && orouter::priceText(nm, false, true) == "~$0.027";
    check(p1 && p2 && p3, "price per picture: per-image exact, token prices by OpenRouter's or the model's own tokens, 2K");
  }
  check(orouter::plausibleKey("sk-or-v1-0123456789abcdef0123") && !orouter::plausibleKey("sk-or-v1 has spaces 0123456") &&
            !orouter::plausibleKey("sk-ant-0123456789012345678"),
        "API key format check");
  int x, y, w, h;
  contextRect(500, 300, 600, 380, 1000, 400, 16.f / 9.f, x, y, w, h);
  check(x >= 0 && y >= 0 && x + w <= 1000 && y + h <= 400 && x <= 500 && x + w >= 600 && y <= 300 && y + h >= 380,
        "context crop stays inside the photo and holds the area");

  // the whole pipeline on a test photo: a red square on blue
  const int W = 600, H = 400;
  std::vector<uint8_t> px((size_t)W * H * 3);
  for (int yy = 0; yy < H; yy++)
    for (int xx = 0; xx < W; xx++) {
      uint8_t* q = &px[((size_t)yy * W + xx) * 3];
      bool sq = xx >= 260 && xx < 340 && yy >= 160 && yy < 240;
      q[0] = sq ? 220 : 40;
      q[1] = sq ? 30 : (uint8_t)(80 + yy / 8);
      q[2] = sq ? 30 : 180;
    }
  std::string photo = dir + "\\gen_photo.png";
  writeImage(photo, px.data(), W, H, true, 95, err);
  GenInput in;
  in.task = GEN_EDIT;
  in.photo = photo;
  in.prompt = "a bird here";
  in.key = "sk-or-v1-test-not-used-000000";
  in.model.id = "test/fake";
  in.model.edits = true;
  in.model.aspects = {"1:1", "4:3", "3:4", "16:9"};
  HealStroke st;
  st.radius = 30.f / W;
  st.pts = {270.f / W, 170.f / H, 330.f / W, 170.f / H, 330.f / W, 230.f / H, 270.f / W, 230.f / H};
  in.mask = {st};
  setGenHook(fakeModel);
  GenOutput out;
  bool ok = runGen(in, out);
  std::vector<uint8_t> rgba;
  int lw = 0, lh = 0;
  bool loaded = ok && out.layers.size() == 1 && loadRgbaFile(out.layers[0].file, rgba, lw, lh);
  check(loaded, (std::string("AI edit makes one layer file") + (out.err.empty() ? "" : ": " + out.err)).c_str());
  if (loaded) {
    const GenLayer& L = out.layers[0];
    check(std::fabs(L.cx - 0.5f) < 0.03f && std::fabs(L.cy - 0.5f) < 0.04f && L.scale > 0.12f && L.scale < 0.3f,
          "the layer sits over the painted area");
    const uint8_t* c = &rgba[((size_t)(lh / 2) * lw + lw / 2) * 4];
    check(c[3] == 255 && c[1] > 150 && c[0] < 90, "inside the area: the model's pixels, fully opaque");
    check(rgba[3] < 10 && rgba[((size_t)lh * lw - 1) * 4 + 3] < 10, "the edge fades out (corners transparent)");
    check(std::fabs(out.cost - 0.01) < 1e-9, "the cost is reported");
  }
  in.task = GEN_MOVE;
  in.prompt = "bird";  // the fake model wants the word
  ok = runGen(in, out);
  bool two = ok && out.layers.size() == 2 && loadRgbaFile(out.layers[1].file, rgba, lw, lh);
  check(two, "move: a background fill and the moved piece");
  if (two) {
    const uint8_t* c = &rgba[((size_t)(lh / 2) * lw + lw / 2) * 4];
    check(c[0] > 150 && c[1] < 90 && c[3] == 255, "the moved piece keeps the original pixels");
  }
  in.task = GEN_EDIT;
  in.model.edits = false;
  check(!runGen(in, out) && !out.err.empty(), "a text-only model is refused for photo edits");
  setGenHook(nullptr);
  // Real lists saved from openrouter.ai, when the tester put them in the folder.
  std::string ri, rp;
  if (readTextFile(dir + "\\or_img_models.json", ri) && readTextFile(dir + "\\or_models.json", rp)) {
    std::vector<orouter::Model> real;
    size_t edits = 0, freeN = 0;
    bool okR = orouter::parseModels(ri, rp, real);
    for (const orouter::Model& m : real) {
      edits += m.edits;
      freeN += m.free;
    }
    printf("  (real OpenRouter list: %d models, %d edit photos, %d free)\n", (int)real.size(), (int)edits, (int)freeN);
    check(okR && real.size() > 20 && edits > 10, "the real OpenRouter model lists parse");
  }
}

// Font list samples: each font draws its own letters (also saves font_samples.png to look at).
void testFontSamples(const std::string& dir) {
  const char* fams[] = {"Sarabun", "Kanit", "Sriracha", "Pacifico", "Bebas Neue", "Press Start 2P"};
  const char* sample = "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5 Aa 123";
  const int H = 40, W = 360;
  std::vector<uint8_t> sheet((size_t)W * H * 6 * 4, 255);
  std::vector<std::vector<uint8_t>> got;
  bool allInk = true;
  for (int i = 0; i < 6; i++) {
    std::vector<uint8_t> al;
    int w = 0;
    bool ok = renderFontSample(fams[i], sample, H, W, al, w);
    size_t ink = 0;
    for (uint8_t v : al) ink += v > 128;
    allInk &= ok && ink > 50 && al.size() == (size_t)w * H;
    got.push_back(al);
    for (int y = 0; y < H && ok; y++)
      for (int x = 0; x < w; x++) {
        uint8_t v = 255 - al[(size_t)y * w + x];
        uint8_t* p = &sheet[(((size_t)i * H + y) * W + x) * 4];
        p[0] = p[1] = p[2] = v;
      }
  }
  check(allInk, "font samples render with ink, at the row height");
  check(got[0] != got[3] && got[1] != got[5], "each font's sample looks different");
  std::string err;
  writePngRgba(dir + "\\font_samples.png", sheet.data(), W, H * 6, err);
}

// Font maker: drawn glyphs -> traced outlines -> a TrueType font Windows can render.
void testFontMaker(const std::string& dir) {
  const int N = kGlyphCell;
  auto blank = [&] { return std::vector<uint8_t>((size_t)N * N, 0); };
  std::vector<uint8_t> O = blank(), I = blank(), mark = blank();
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      float d = std::hypot(x - 128.f, y - 120.f);
      if (d < 78 && d > 44) O[(size_t)y * N + x] = 255;  // a ring: one outer contour, one hole
      if (x >= 112 && x < 144 && y >= kGlyphCapHeight && y < kGlyphBaseline) I[(size_t)y * N + x] = 255;
      if (x >= 120 && x < 136 && y >= 10 && y < 30) mark[(size_t)y * N + x] = 255;
    }
  auto loops = traceBitmap(O, N, N);
  check(loops.size() == 2, "a ring traces to two contours (outside and hole)");
  std::vector<FmGlyph> G(1);
  G[0].advance = 600;
  G[0].contours = {{{60, 0}, {60, 700}, {540, 700}, {540, 0}}};
  const float k = 1100.f / N;
  auto toGlyph = [&](uint32_t cp, const std::vector<uint8_t>& ink, bool zeroWidth) {
    FmGlyph g;
    g.cp = cp;
    auto L = traceBitmap(ink, N, N);
    int x0 = 1 << 20, x1 = -(1 << 20);
    for (auto& c : L) {
      std::vector<std::pair<int, int>> m;
      for (auto& q : c) {
        m.push_back({(int)std::lround(q.first * k), (int)std::lround((kGlyphBaseline - q.second) * k)});
        x0 = std::min(x0, m.back().first);
        x1 = std::max(x1, m.back().second > -100000 ? m.back().first : x1);
      }
      g.contours.push_back(m);
    }
    // wind: largest contour clockwise, the rest the other way (good enough for these shapes)
    size_t big = 0;
    auto area = [](const std::vector<std::pair<int, int>>& c) {
      long long s = 0;
      for (size_t a = 0; a < c.size(); a++) s += (long long)c[a].first * c[(a + 1) % c.size()].second - (long long)c[(a + 1) % c.size()].first * c[a].second;
      return s;
    };
    for (size_t a = 1; a < g.contours.size(); a++)
      if (std::llabs(area(g.contours[a])) > std::llabs(area(g.contours[big]))) big = a;
    for (size_t a = 0; a < g.contours.size(); a++)
      if ((area(g.contours[a]) < 0) != (a == big)) std::reverse(g.contours[a].begin(), g.contours[a].end());
    for (auto& c : g.contours)
      for (auto& q : c) q.first += zeroWidth ? -300 : 60 - x0;
    g.advance = zeroWidth ? 0 : (x1 - x0) + 120;
    return g;
  };
  G.push_back(toGlyph('I', I, false));
  G.push_back(toGlyph('O', O, false));
  G.push_back(toGlyph(0x0E48, mark, true));
  std::vector<uint8_t> ttf = writeTrueType("CR Selftest Font", G);
  std::string file = dir + "\\selftest_font.ttf";
  writeBytes(file, ttf);
  check(ttf.size() > 500 && loadPrivateFont(file) && fontAvailable("CR Selftest Font"), "the made font loads into Windows");
  TextSpec t;
  t.font = "CR Selftest Font";
  t.text = "OI";
  t.color = 0x000000FF;
  std::vector<uint8_t> rgb, al;
  int w = 0, h = 0;
  std::string err;
  bool ok = renderText(t, rgb, al, w, h, err);
  int ink = 0;
  for (uint8_t v : al) ink += v > 128;
  check(ok && ink > 1000 && w > h, "text renders in the made font");
  // The O's hole must stay empty: scan the row through the O's middle for ink, gap, ink.
  bool hole = false;
  if (ok) {
    int y = h / 2, transitions = 0;
    bool prev = false;
    for (int x = 0; x < w; x++) {
      bool on = al[(size_t)y * w + x] > 128;
      transitions += on != prev;
      prev = on;
    }
    hole = transitions >= 6;  // O left edge, hole, O right edge, I: six changes along the row
  }
  check(hole, "contours are wound so the O keeps its hole");
}

// Camera RAW files come from anywhere: damaged or crafted ones must fail cleanly.
void testRaw(const std::string& dir) {
  check(isRawFile("C:\\p\\IMG_0001.CR2") && isRawFile("a.nef") && isRawFile("b.Dng") && isRawFile("c.cr3") &&
            !isRawFile("a.jpg") && !isRawFile("folder.nef\\photo.jpg") && !isRawFile("noext"),
        "RAW files are recognised by extension");
  ImageData img;
  std::string err;
  std::string junk = dir + "\\junk.nef";
  writeBytes(junk, std::vector<uint8_t>(4096, 0x5A));
  check(!loadImage(junk, 8192, img, err) && img.px16.empty(), ("a non-RAW file named .nef is refused: " + err).c_str());
  ai::SmallImage si;
  check(!ai::decodeScaled(junk, 512, si, nullptr, err), "the AI decoder refuses it too");
  // A TIFF header (what CR2, NEF, ARW and DNG start with) whose first directory points past the end.
  std::vector<uint8_t> tiff = {'I', 'I', 42, 0, 0xF0, 0xFF, 0xFF, 0x7F};
  tiff.resize(2048, 0);
  std::string fake = dir + "\\truncated.dng";
  writeBytes(fake, tiff);
  check(!loadImage(fake, 8192, img, err), "a truncated TIFF-based RAW fails cleanly");
  std::string empty = dir + "\\empty.cr2";
  writeBytes(empty, std::vector<uint8_t>());
  check(!loadImage(empty, 8192, img, err), "an empty RAW file fails cleanly");
}

void testUpdater(const std::string& dir) {
  check(compareVersions("1.3.0", "1.2.1") > 0 && compareVersions("1.2.10", "1.2.9") > 0 &&
            compareVersions("1.2", "1.2.0") == 0 && compareVersions("1.2.1", "1.10.0") < 0,
        "versions compare by number, not by text");
  check(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 is correct");
  check(crc32Of("123456789", 9) == 0xCBF43926u, "CRC-32 is correct");
  std::string rel;
  check(zipEntryPath("CircleRoom/ai/model.onnx", rel) && rel == "ai\\model.onnx", "release zip paths map into the program folder");
  check(!zipEntryPath("CircleRoom/../evil.exe", rel) && !zipEntryPath("CircleRoom/ai/../../x", rel) &&
            !zipEntryPath("Other/x.exe", rel) && !zipEntryPath("CircleRoom/C:/x", rel) &&
            !zipEntryPath("CircleRoom/a\\..\\x", rel),
        "zip entries cannot escape the program folder");
  check(!zipEntryPath("CircleRoom/settings.ini", rel) && !zipEntryPath("CircleRoom/presets/mine.plook", rel),
        "an update never overwrites settings or presets");
  ReleaseInfo r;
  std::string err;
  const char* feed =
      "{\"tag_name\":\"v1.3.0\",\"html_url\":\"https://github.com/Godpoom/circle-room/releases/tag/v1.3.0\","
      "\"body\":\"## New\\n- **Batch** edit\",\"assets\":[{\"name\":\"CircleRoom_Lite.zip\",\"size\":10,"
      "\"browser_download_url\":\"https://x/lite.zip\"},{\"name\":\"CircleRoom_Portable.zip\",\"size\":20,"
      "\"browser_download_url\":\"https://x/full.zip\"},{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://x/s\"}]}";
  check(parseRelease(feed, false, r, err) && r.version == "1.3.0" && r.assetUrl == "https://x/full.zip" &&
            r.assetSize == 20 && r.sumsUrl == "https://x/s" && r.notes.find("Batch edit") != std::string::npos,
        "release info picks the Portable zip for the full build");
  check(parseRelease(feed, true, r, err) && r.assetUrl == "https://x/lite.zip", "and the Lite zip for Lite");
  check(!parseRelease("{\"tag_name\":\"v1.3.0; rm\"}", false, r, err), "a malformed version is refused");
  std::string zp = dir + "\\upd.zip";
  std::vector<uint8_t> zb = storedZip({{"CircleRoom/CircleRoom.exe", "new exe"}, {"CircleRoom/ai/m.onnx", "model"}});
  writeBytes(zp, zb);
  std::vector<ZipEntry> es;
  std::vector<uint8_t> data;
  check(zipList(zp, es, err) && es.size() == 2 && zipRead(zp, es[1], data, err) &&
            std::string(data.begin(), data.end()) == "model",
        "zip reader lists and extracts");
  zb[30 + 25 + 1] ^= 0xFF;  // flip a byte of the first file's data (after its 30-byte header and name)
  writeBytes(zp, zb);
  check(zipList(zp, es, err) && !zipRead(zp, es[0], data, err), "a corrupted zip entry fails its CRC check");
  writeBytes(zp, std::vector<uint8_t>(100, 0));
  check(!zipList(zp, es, err), "a file that is not a zip is refused");
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
  testAgentLog();
  testUpdater(dir);
  testRaw(dir);
  testFontSamples(dir);
  testAiGen(dir);
  testFontMaker(dir);
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
