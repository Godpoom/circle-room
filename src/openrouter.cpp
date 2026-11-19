#include "openrouter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include "mcp_json.h"
#include "platform_win.h"
#include "version.h"

namespace orouter {
namespace {

std::string appDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "";
  std::string d = wideToUtf8(buf) + "\\CircleRoom";
  ensureDir(d);
  return d;
}

// Requests in flight, per job, so cancel() can close them (closing wakes a blocked send / read).
// The flags are reset by resetCancel() on the UI thread before a job starts, never by the job:
// a Cancel pressed right after Generate can then never be lost.
std::mutex g_reqM;
struct Live {
  HINTERNET h;
  int job;
};
std::vector<Live> g_live;
std::atomic<bool> g_cancel[2] = {{false}, {false}};

// HTTPS only. body: request body for POST (empty = GET). Returns false on network errors;
// status holds the HTTP status otherwise.
bool https(const wchar_t* method, const std::wstring& host, const std::wstring& path, const std::string& headers,
           const std::string& body, std::string& resp, int& status, int receiveMs, std::string& err, int job) {
  std::atomic<bool>& cancelled = g_cancel[job];
  resp.clear();
  status = 0;
  std::wstring agent = utf8ToWide(std::string("CircleRoom/") + CR_VERSION_STR);
  HINTERNET ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    err = "network unavailable";
    return false;
  }
  WinHttpSetTimeouts(ses, 15000, 15000, 60000, receiveMs);
  HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET req = con ? WinHttpOpenRequest(con, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE)
                      : nullptr;
  if (req) {
    std::lock_guard<std::mutex> lk(g_reqM);
    g_live.push_back({req, job});
  }
  bool ok = false;
  if (req && !cancelled) {
    std::wstring hdr = utf8ToWide(headers);
    BOOL sent = WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), (DWORD)-1L,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                                   (DWORD)body.size(), 0);
    if (sent && WinHttpReceiveResponse(req, nullptr)) {
      DWORD st = 0, sz = sizeof(st);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
                          WINHTTP_NO_HEADER_INDEX);
      status = (int)st;
      std::vector<char> buf(1 << 16);
      ok = true;
      for (;;) {
        DWORD rd = 0;
        if (cancelled || !WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &rd)) {
          ok = false;
          break;
        }
        if (rd == 0) break;
        if (resp.size() + rd > (size_t)96 << 20) {
          err = "response too large";
          ok = false;
          break;
        }
        resp.append(buf.data(), rd);
      }
    }
    if (!ok && err.empty()) {
      DWORD e = GetLastError();
      err = cancelled ? "cancelled" : e == ERROR_WINHTTP_TIMEOUT ? "the AI took too long to answer (timeout)"
                                                                 : "network error " + std::to_string(e);
    }
  } else {
    err = cancelled ? "cancelled" : "network unavailable";
  }
  if (req) {  // close it unless cancel() already did
    std::lock_guard<std::mutex> lk(g_reqM);
    for (size_t i = 0; i < g_live.size(); i++)
      if (g_live[i].h == req) {
        WinHttpCloseHandle(req);
        g_live.erase(g_live.begin() + i);
        break;
      }
  }
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  return ok;
}

std::string apiError(const std::string& body, int status) {
  Json j;
  std::string m;
  if (json::parse(body, j)) {
    const Json* e = j.get("error");
    if (e && e->isObj()) m = e->str("message");
    else if (e && e->isStr()) m = e->s;
  }
  if (status == 401) return "API key not accepted (401)" + (m.empty() ? "" : ": " + m);
  if (status == 402) return "not enough OpenRouter credit for this model (402)" + (m.empty() ? "" : ": " + m);
  if (status == 429) return "too many requests, try again shortly (429)" + (m.empty() ? "" : ": " + m);
  if (m.empty()) m = body.substr(0, 200);
  return "OpenRouter " + std::to_string(status) + ": " + m;
}

std::vector<std::string> enumValues(const Json* sp, const char* name) {
  std::vector<std::string> v;
  const Json* p = sp ? sp->get(name) : nullptr;
  const Json* vals = p ? p->get("values") : nullptr;
  if (vals && vals->isArr())
    for (const Json& x : vals->a)
      if (x.isStr()) v.push_back(x.s);
  return v;
}

bool hasParam(const Json* sp, const char* name) {
  if (!sp) return false;
  if (sp->isObj()) return sp->get(name) != nullptr;
  if (sp->isArr())
    for (const Json& x : sp->a)
      if (x.isStr() && x.s == name) return true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------- key
bool plausibleKey(const std::string& k) {
  if (k.size() < 20 || k.size() > 400 || k.compare(0, 6, "sk-or-") != 0) return false;
  for (char c : k)
    if ((unsigned char)c <= ' ' || (unsigned char)c >= 127) return false;
  return true;
}

bool saveKey(const std::string& key, std::string& err) {
  std::string dir = appDir();
  if (dir.empty()) {
    err = "no app data folder";
    return false;
  }
  DATA_BLOB in{(DWORD)key.size(), (BYTE*)key.data()}, out{};
  if (!CryptProtectData(&in, L"Circle Room OpenRouter key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    err = "cannot encrypt the key";
    return false;
  }
  std::string blob((const char*)out.pbData, out.cbData);
  LocalFree(out.pbData);
  if (!writeTextFile(dir + "\\openrouter.key", blob)) {
    err = "cannot save the key";
    return false;
  }
  return true;
}

bool loadKey(std::string& key) {
  key.clear();
  std::string dir = appDir(), blob;
  if (dir.empty() || !readTextFile(dir + "\\openrouter.key", blob) || blob.empty()) return false;
  DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
  key.assign((const char*)out.pbData, out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return plausibleKey(key);
}

void clearKey() {
  std::string dir = appDir();
  if (!dir.empty()) deleteFileU(dir + "\\openrouter.key");
}

// ---------------------------------------------------------------- models
static void sortModels(std::vector<Model>& out) {  // free, then cheapest, unknown prices last
  std::stable_sort(out.begin(), out.end(), [](const Model& a, const Model& b) {
    if (a.free != b.free) return a.free;
    bool ka = a.outCost >= 0, kb = b.outCost >= 0;
    if (ka != kb) return ka;
    if (ka && a.outCost != b.outCost) return a.outCost < b.outCost;
    return a.name < b.name;
  });
}

bool parseModels(const std::string& imagesJson, const std::string& pricesJson, std::vector<Model>& out) {
  out.clear();
  Json im, pr;
  if (!json::parse(imagesJson, im)) return false;
  const Json* data = im.get("data");
  if (!data || !data->isArr()) return false;
  std::vector<Model> caps;
  for (const Json& m : data->a) {
    Model x;
    x.id = m.str("id");
    x.name = m.str("name", x.id);
    if (x.id.empty() || x.id.size() > 200) continue;
    const Json* arch = m.get("architecture");
    const Json* inm = arch ? arch->get("input_modalities") : nullptr;
    bool imgIn = false;
    if (inm && inm->isArr())
      for (const Json& v : inm->a) imgIn |= v.isStr() && v.s == "image";
    const Json* sp = m.get("supported_parameters");
    x.edits = imgIn && hasParam(sp, "input_references");
    x.aspects = enumValues(sp, "aspect_ratio");
    x.resolutions = enumValues(sp, "resolution");
    x.formats = enumValues(sp, "output_format");
    caps.push_back(x);
  }
  auto capOf = [&](const std::string& id) -> const Model* {
    for (const Model& c : caps)
      if (c.id == id) return &c;
    return nullptr;
  };
  std::vector<std::string> seen;
  if (json::parse(pricesJson, pr) && pr.get("data") && pr.get("data")->isArr()) {
    for (const Json& m : pr.get("data")->a) {
      std::string id = m.str("id");
      if (id.empty() || id.compare(0, 11, "openrouter/") == 0) continue;
      bool freeTag = id.size() > 5 && id.compare(id.size() - 5, 5, ":free") == 0;
      const Model* c = capOf(freeTag ? id.substr(0, id.size() - 5) : id);
      if (!c) continue;  // not served by the image API
      Model x = *c;
      x.id = id;
      x.name = m.str("name", c->name);
      bool allZero = true;
      const Json* p = m.get("pricing");
      if (p && p->isObj())
        for (const auto& kv : p->o) {
          double v = kv.second.isStr() ? atof(kv.second.s.c_str()) : kv.second.isNum() ? kv.second.n : 0;
          if (v != 0) allZero = false;
          if (kv.first == "image_token") x.normTok = true;  // OpenRouter's own image tokens (4175 per 1K picture)
        }
      x.free = freeTag || allZero;
      out.push_back(x);
      seen.push_back(id);
    }
  }
  for (const Model& c : caps)
    if (std::find(seen.begin(), seen.end(), c.id) == seen.end()) out.push_back(c);
  for (Model& m : out)
    if (m.free) m.outCost = m.inCost = 0;
  sortModels(out);
  return !out.empty();
}

bool parseEndpointPrice(const std::string& text, Model& m) {
  Json j;
  if (!json::parse(text, j)) return false;
  const Json* eps = j.get("endpoints");
  if (!eps || !eps->isArr()) return false;
  bool any = false;
  for (const Json& e : eps->a) {
    const Json* pr = e.get("pricing");
    if (!pr || !pr->isArr()) continue;
    double outBase = -1, outVar = -1, out2K = -1, in = 0;  // plain price, cheapest variant, high-resolution variant
    bool approx = false, scale2K = false;
    for (const Json& p : pr->a) {
      std::string what = p.str("billable"), unit = p.str("unit");
      double c = p.num("cost_usd", -1);
      if (c < 0) continue;
      bool variant = !p.str("variant").empty();
      double per = c;
      bool est = false;
      if (unit == "token") {  // tokens per 1K picture: OpenRouter's image tokens, or the model's own
        per = c * (m.normTok ? 4175.0 : what == "output_image" ? 1290.0 : 560.0);
        est = true;
        if (what == "output_image") scale2K = true;
      } else if (unit == "megapixel") {
        per = c * 1.05;
        est = true;
        if (what == "output_image") scale2K = true;
      } else if (unit != "image") {
        continue;
      }
      if (what == "output_image") {
        std::string v = p.str("variant");
        for (char& ch : v) ch = (char)tolower((unsigned char)ch);
        if (!variant && outBase < 0) outBase = per;
        else if (variant && (v.find("2k") != std::string::npos || v.find("high") != std::string::npos)) out2K = per;
        else if (variant && (outVar < 0 || per < outVar)) outVar = per;
        approx |= est;
      } else if (what == "input_image" && !variant) {
        in = per;
        approx |= est;
      }
    }
    double out = outBase >= 0 ? outBase : outVar;
    if (out < 0) continue;
    if (out2K < 0) out2K = scale2K ? out * (m.normTok ? 4.0 : 1.3) : out;  // by pixels, or the same price
    if (!any || out + in < m.outCost + m.inCost) {
      m.outCost = out;
      m.out2K = out2K;
      m.inCost = in;
      m.approx = approx;
      any = true;
    }
  }
  return any;
}

static std::string money(double c, bool approx) {
  char b[40];
  if (c < 0.0995) snprintf(b, sizeof(b), "%s$%.3f", approx ? "~" : "", c);
  else snprintf(b, sizeof(b), "%s$%.2f", approx ? "~" : "", c);
  return b;
}

std::string priceText(const Model& m, bool edit, bool hiRes, bool* paid) {
  if (paid) *paid = false;
  if (m.free) return "";
  bool hi = hiRes && !m.resolutions.empty();
  double seen = paidCost(m.id, edit, hi);
  if (seen >= 0) {  // what this model really cost last time
    if (paid) *paid = true;
    return money(seen, false);
  }
  if (m.outCost < 0) return "";
  double out = hi && m.out2K >= 0 ? m.out2K : m.outCost;
  return money(out + (edit ? 2 * m.inCost : 0), m.approx);
}

// ---------------------------------------------------------------- what results really cost
static std::mutex g_costM;
static std::string costKey(const std::string& id, bool edit, bool hi) { return id + (edit ? "|edit" : "|make") + (hi ? "|2K" : "|1K"); }

static Json& costs() {
  static Json c;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    std::string dir = appDir(), t;
    if (dir.empty() || !readTextFile(dir + "\\openrouter_costs.json", t) || !json::parse(t, c) || !c.isObj()) c = Json::object();
  }
  return c;
}

double paidCost(const std::string& id, bool edit, bool hi) {
  std::lock_guard<std::mutex> lk(g_costM);
  return costs().num(costKey(id, edit, hi).c_str(), -1);
}

void rememberCost(const std::string& id, bool edit, bool hi, double usd) {
  if (usd < 0 || !std::isfinite(usd)) return;
  std::lock_guard<std::mutex> lk(g_costM);
  costs().set(costKey(id, edit, hi), usd);
  std::string dir = appDir();
  if (!dir.empty()) writeTextFile(dir + "\\openrouter_costs.json", json::dump(costs()));
}

bool fetchModels(std::vector<Model>& out, std::string& err) {
  std::string a, b, e2;
  int st = 0;
  const char* hdr = "Accept: application/json\r\n";
  if (!https(L"GET", L"openrouter.ai", L"/api/v1/images/models", hdr, "", a, st, 30000, err, kModelList)) return false;
  if (st != 200) {
    err = apiError(a, st);
    return false;
  }
  if (!https(L"GET", L"openrouter.ai", L"/api/v1/models?output_modalities=image", hdr, "", b, st, 30000, e2, kModelList) || st != 200)
    b.clear();  // prices are a bonus: the list still works without them
  if (!parseModels(a, b, out)) {
    err = "unexpected model list from OpenRouter";
    return false;
  }
  // Exact prices per picture from each model's endpoints (a few dozen small requests, 8 at a time).
  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (size_t i; (i = next++) < out.size();) {
      Model& m = out[i];
      if (m.free || g_cancel[kModelList]) continue;
      std::string id = m.id, body, e;
      if (id.size() > 5 && id.compare(id.size() - 5, 5, ":free") == 0) id.resize(id.size() - 5);
      int s = 0;
      if (https(L"GET", L"openrouter.ai", utf8ToWide("/api/v1/images/models/" + id + "/endpoints"), hdr, "", body, s, 20000, e,
                kModelList) &&
          s == 200)
        parseEndpointPrice(body, m);
    }
  };
  std::vector<std::thread> th;
  for (int k = 0; k < 8; k++) th.emplace_back(worker);
  for (std::thread& t : th) t.join();
  sortModels(out);
  return true;
}

void saveModelCache(const std::vector<Model>& ms) {
  Json arr = Json::array();
  for (const Model& m : ms) {
    Json o = Json::object();
    o.set("id", m.id);
    o.set("name", m.name);
    o.set("free", m.free);
    o.set("edits", m.edits);
    o.set("outCost", m.outCost);
    o.set("inCost", m.inCost);
    o.set("out2K", m.out2K);
    o.set("normTok", m.normTok);
    o.set("approx", m.approx);
    auto list = [](const std::vector<std::string>& v) {
      Json a = Json::array();
      for (const std::string& s : v) a.push(s);
      return a;
    };
    o.set("aspects", list(m.aspects));
    o.set("resolutions", list(m.resolutions));
    o.set("formats", list(m.formats));
    arr.push(o);
  }
  Json root = Json::object();
  root.set("time", (double)time(nullptr));
  root.set("v", 3);
  root.set("models", arr);
  std::string dir = appDir();
  if (!dir.empty()) writeTextFile(dir + "\\openrouter_models.json", json::dump(root));
}

bool loadModelCache(std::vector<Model>& out, long long* ageSec) {
  out.clear();
  std::string dir = appDir(), text;
  Json root;
  if (dir.empty() || !readTextFile(dir + "\\openrouter_models.json", text) || !json::parse(text, root)) return false;
  const Json* ms = root.get("models");
  if (!ms || !ms->isArr() || root.num("v", 1) < 3) return false;  // older cache: no prices
  auto list = [](const Json* a) {
    std::vector<std::string> v;
    if (a && a->isArr())
      for (const Json& x : a->a)
        if (x.isStr()) v.push_back(x.s);
    return v;
  };
  for (const Json& o : ms->a) {
    Model m;
    m.id = o.str("id");
    m.name = o.str("name", m.id);
    m.free = o.flag("free", false);
    m.edits = o.flag("edits", false);
    m.outCost = o.num("outCost", -1);
    m.inCost = o.num("inCost", 0);
    m.out2K = o.num("out2K", -1);
    m.normTok = o.flag("normTok", false);
    m.approx = o.flag("approx", false);
    m.aspects = list(o.get("aspects"));
    m.resolutions = list(o.get("resolutions"));
    m.formats = list(o.get("formats"));
    if (!m.id.empty()) out.push_back(m);
  }
  if (ageSec) *ageSec = (long long)time(nullptr) - (long long)root.num("time", 0);
  return !out.empty();
}

std::string loadChosenModel() {
  std::string dir = appDir(), t;
  if (dir.empty() || !readTextFile(dir + "\\openrouter_model.txt", t)) return "";
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  return t.size() < 200 ? t : "";
}

void saveChosenModel(const std::string& id) {
  std::string dir = appDir();
  if (!dir.empty()) writeTextFile(dir + "\\openrouter_model.txt", id);
}

std::string nearestAspect(const Model& m, float ratio, float* exact) {
  std::string best;
  float bestD = 1e9f, bestR = ratio;
  for (const std::string& a : m.aspects) {
    float w = 0, h = 0;
    if (sscanf(a.c_str(), "%f:%f", &w, &h) != 2 || w <= 0 || h <= 0) continue;
    float d = std::fabs(std::log((w / h) / ratio));
    if (d < bestD) {
      bestD = d;
      best = a;
      bestR = w / h;
    }
  }
  if (exact) *exact = bestR;
  return best;
}

// ---------------------------------------------------------------- generation
std::string base64(const uint8_t* d, size_t n) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    o += T[v >> 18 & 63];
    o += T[v >> 12 & 63];
    o += i + 1 < n ? T[v >> 6 & 63] : '=';
    o += i + 2 < n ? T[v & 63] : '=';
  }
  return o;
}

bool unbase64(const std::string& s, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(s.size() / 4 * 3);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=' || c == '\n' || c == '\r') continue;
    else return false;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t)(acc >> bits & 255));
    }
  }
  return !out.empty();
}

std::string buildRequestJson(const Request& rq) {
  Json j = Json::object();
  j.set("model", rq.model);
  j.set("prompt", rq.prompt);
  j.set("n", 1);
  if (!rq.aspect.empty()) j.set("aspect_ratio", rq.aspect);
  if (!rq.resolution.empty()) j.set("resolution", rq.resolution);
  if (!rq.format.empty()) j.set("output_format", rq.format);
  if (!rq.images.empty()) {
    Json refs = Json::array();
    for (const Input& in : rq.images) {
      Json u = Json::object();
      u.set("url", "data:" + in.mime + ";base64," + base64(in.bytes.data(), in.bytes.size()));
      Json r = Json::object();
      r.set("type", "image_url");
      r.set("image_url", u);
      refs.push(r);
    }
    j.set("input_references", refs);
  }
  return json::dump(j);
}

bool parseResult(const std::string& body, Result& out, std::string& err) {
  Json j;
  if (!json::parse(body, j)) {
    err = "unexpected answer from OpenRouter";
    return false;
  }
  const Json* data = j.get("data");
  if (!data || !data->isArr() || data->a.empty()) {
    err = "the model returned no image (it may have refused the prompt)";
    return false;
  }
  const Json& d0 = data->a[0];
  std::string b = d0.str("b64_json");
  if (b.empty()) {
    std::string url = d0.str("url");
    size_t comma = url.find(',');
    if (url.compare(0, 5, "data:") == 0 && comma != std::string::npos) b = url.substr(comma + 1);
  }
  if (b.empty() || !unbase64(b, out.bytes)) {
    err = "the model returned no image data";
    return false;
  }
  out.mime = d0.str("media_type", "image/png");
  const Json* u = j.get("usage");
  out.cost = u ? u->num("cost", -1) : -1;
  return true;
}

bool generate(const std::string& key, const Request& rq, Result& out, std::string& err) {
  if (!plausibleKey(key)) {
    err = "no OpenRouter API key";
    return false;
  }
  std::string body = buildRequestJson(rq), resp;
  std::string hdr = "Authorization: Bearer " + key +
                    "\r\nContent-Type: application/json\r\nAccept: application/json\r\n"
                    "HTTP-Referer: https://godpoom.github.io/circle-room/\r\nX-Title: Circle Room\r\n";
  int st = 0;
  bool ok = https(L"POST", L"openrouter.ai", L"/api/v1/images", hdr, body, resp, st, 300000, err, kGenerate);
  SecureZeroMemory(&hdr[0], hdr.size());
  if (!ok) return false;
  if (st != 200) {
    err = apiError(resp, st);
    return false;
  }
  return parseResult(resp, out, err);
}

void resetCancel(Job job) { g_cancel[job] = false; }

void cancel(Job job) {
  g_cancel[job] = true;
  std::lock_guard<std::mutex> lk(g_reqM);
  for (size_t i = 0; i < g_live.size();)
    if (g_live[i].job == job) {
      WinHttpCloseHandle(g_live[i].h);  // wakes a send / read that is waiting on the network
      g_live.erase(g_live.begin() + i);
    } else {
      i++;
    }
}

bool cancelled(Job job) { return g_cancel[job]; }

void cancelAll() {
  cancel(kGenerate);
  cancel(kModelList);
}

bool parseKeyInfo(const std::string& body, KeyInfo& out) {
  Json j;
  if (!json::parse(body, j)) return false;
  const Json* d = j.get("data");
  if (!d || !d->isObj()) return false;
  out = KeyInfo();
  out.usage = d->num("usage", -1);
  out.limit = d->num("limit", -1);
  out.remaining = d->num("limit_remaining", -1);
  out.freeTier = d->flag("is_free_tier", false);
  if (const Json* f = d->get("free_model_daily_requests")) out.freeLeft = (int)f->num("remaining", -1);
  return true;
}

bool keyInfo(const std::string& key, KeyInfo& out, std::string& err) {
  if (!plausibleKey(key)) {
    err = "API key not accepted (401)";
    return false;
  }
  std::string hdr = "Authorization: Bearer " + key + "\r\nAccept: application/json\r\n", body;
  int st = 0;
  bool ok = https(L"GET", L"openrouter.ai", L"/api/v1/key", hdr, "", body, st, 20000, err, kModelList);
  SecureZeroMemory(&hdr[0], hdr.size());
  if (!ok) return false;
  if (st != 200) {
    err = apiError(body, st);
    return false;
  }
  if (!parseKeyInfo(body, out)) {
    err = "unexpected answer from OpenRouter";
    return false;
  }
  return true;
}

std::string maskKey(const std::string& key) {
  if (key.size() < 14) return "sk-or-...";
  return key.substr(0, 9) + "..." + key.substr(key.size() - 4);
}

int errorKind(const std::string& e) {
  auto has = [&](const char* s) { return e.find(s) != std::string::npos; };
  if (e.empty()) return ERR_NONE;
  if (has("cancelled")) return ERR_CANCELLED;
  if (has("(401)") || has("(403)") || has("no OpenRouter API key")) return ERR_KEY;
  if (has("(402)")) return ERR_CREDIT;
  if (has("(429)") || has("OpenRouter 503") || has("OpenRouter 502")) return ERR_BUSY;
  if (has("timeout")) return ERR_TIMEOUT;
  if (has("network")) return ERR_NETWORK;
  if (has("returned no image") || has("no image data") || has("refused")) return ERR_REFUSED;
  if (has("no blown-out area")) return ERR_NO_CLIP;  // before "paint over": its message says both
  if (has("paint over")) return ERR_NEED_AREA;
  if (has("cannot edit photos")) return ERR_NEED_EDITS;
  return ERR_OTHER;
}

void openCreditsPage() { ShellExecuteW(nullptr, L"open", L"https://openrouter.ai/settings/credits", nullptr, nullptr, SW_SHOWNORMAL); }

void openKeysPage() { ShellExecuteW(nullptr, L"open", L"https://openrouter.ai/keys", nullptr, nullptr, SW_SHOWNORMAL); }
void wipe(void* p, size_t n) { SecureZeroMemory(p, n); }

}  // namespace orouter
