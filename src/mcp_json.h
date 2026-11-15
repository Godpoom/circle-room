// Minimal JSON value, parser and writer for the MCP server (JSON-RPC over stdio).
// Input comes from another program, so the parser is strict, bounded in depth and never throws.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  Json() = default;
  Json(bool v) : t(Bool), b(v) {}
  Json(int v) : t(Num), n(v) {}
  Json(double v) : t(Num), n(v) {}
  Json(const char* v) : t(Str), s(v) {}
  Json(std::string v) : t(Str), s(std::move(v)) {}
  static Json array() { Json j; j.t = Arr; return j; }
  static Json object() { Json j; j.t = Obj; return j; }

  bool isNull() const { return t == Null; }
  bool isNum() const { return t == Num; }
  bool isStr() const { return t == Str; }
  bool isArr() const { return t == Arr; }
  bool isObj() const { return t == Obj; }
  const Json* get(const char* k) const {
    if (t != Obj) return nullptr;
    for (const auto& kv : o)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
  Json& set(const std::string& k, Json v) {
    t = Obj;
    for (auto& kv : o)
      if (kv.first == k) return kv.second = std::move(v);
    o.emplace_back(k, std::move(v));
    return o.back().second;
  }
  Json& push(Json v) {
    t = Arr;
    a.push_back(std::move(v));
    return a.back();
  }
  // Typed lookups with defaults (wrong type = default).
  double num(const char* k, double def) const {
    const Json* v = get(k);
    return v && v->t == Num && std::isfinite(v->n) ? v->n : def;
  }
  std::string str(const char* k, const std::string& def = "") const {
    const Json* v = get(k);
    return v && v->t == Str ? v->s : def;
  }
  bool flag(const char* k, bool def) const {
    const Json* v = get(k);
    return v && v->t == Bool ? v->b : def;
  }
  bool has(const char* k) const { return get(k) != nullptr; }
};

namespace json {

namespace detail {
struct P {
  const char* p;
  const char* e;
  int depth = 0;
  bool ok = true;
  void ws() {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }
  bool lit(const char* w) {
    const char* q = p;
    for (; *w; w++, q++)
      if (q >= e || *q != *w) return false;
    p = q;
    return true;
  }
  static void utf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) out += (char)(0xC0 | (c >> 6)), out += (char)(0x80 | (c & 63));
    else if (c < 0x10000) out += (char)(0xE0 | (c >> 12)), out += (char)(0x80 | ((c >> 6) & 63)), out += (char)(0x80 | (c & 63));
    else out += (char)(0xF0 | (c >> 18)), out += (char)(0x80 | ((c >> 12) & 63)), out += (char)(0x80 | ((c >> 6) & 63)), out += (char)(0x80 | (c & 63));
  }
  bool hex4(uint32_t& v) {
    if (e - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++, p++) {
      char c = *p;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (p >= e || *p != '"') return false;
    p++;
    while (p < e && *p != '"') {
      char c = *p++;
      if ((unsigned char)c < 0x20) return false;
      if (c != '\\') { out += c; continue; }
      if (p >= e) return false;
      char x = *p++;
      switch (x) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t c1;
          if (!hex4(c1)) return false;
          if (c1 >= 0xD800 && c1 < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            uint32_t c2;
            if (!hex4(c2)) return false;
            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
          }
          utf8(out, c1);
          break;
        }
        default: return false;
      }
    }
    if (p >= e) return false;
    p++;
    return true;
  }
  Json value() {
    Json j;
    ws();
    if (p >= e || ++depth > 64) { ok = false; return j; }
    char c = *p;
    if (c == '{') {
      p++;
      j = Json::object();
      ws();
      if (p < e && *p == '}') { p++; depth--; return j; }
      while (ok) {
        ws();
        std::string k;
        if (!string(k)) { ok = false; break; }
        ws();
        if (p >= e || *p != ':') { ok = false; break; }
        p++;
        j.o.emplace_back(std::move(k), value());
        ws();
        if (p < e && *p == ',') { p++; continue; }
        if (p < e && *p == '}') { p++; break; }
        ok = false;
      }
    } else if (c == '[') {
      p++;
      j = Json::array();
      ws();
      if (p < e && *p == ']') { p++; depth--; return j; }
      while (ok) {
        j.a.push_back(value());
        ws();
        if (p < e && *p == ',') { p++; continue; }
        if (p < e && *p == ']') { p++; break; }
        ok = false;
      }
    } else if (c == '"') {
      j.t = Json::Str;
      if (!string(j.s)) ok = false;
    } else if (lit("true")) {
      j = Json(true);
    } else if (lit("false")) {
      j = Json(false);
    } else if (lit("null")) {
    } else {
      char* end = nullptr;
      std::string tmp(p, std::min<size_t>(64, e - p));
      double v = strtod(tmp.c_str(), &end);
      if (end == tmp.c_str()) { ok = false; return j; }
      p += end - tmp.c_str();
      j = Json(v);
    }
    depth--;
    return j;
  }
};

inline void esc(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          out += b;
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
}
}  // namespace detail

inline bool parse(const std::string& text, Json& out) {
  detail::P p{text.data(), text.data() + text.size()};
  out = p.value();
  p.ws();
  return p.ok && p.p == p.e;
}

inline void write(const Json& j, std::string& out) {
  switch (j.t) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: {
      if (!std::isfinite(j.n)) { out += "null"; break; }
      char b[32];
      if (j.n == (double)(long long)j.n && std::fabs(j.n) < 1e15) snprintf(b, sizeof(b), "%lld", (long long)j.n);
      else snprintf(b, sizeof(b), "%.6g", j.n);
      out += b;
      break;
    }
    case Json::Str: detail::esc(out, j.s); break;
    case Json::Arr:
      out += '[';
      for (size_t i = 0; i < j.a.size(); i++) {
        if (i) out += ',';
        write(j.a[i], out);
      }
      out += ']';
      break;
    case Json::Obj:
      out += '{';
      for (size_t i = 0; i < j.o.size(); i++) {
        if (i) out += ',';
        detail::esc(out, j.o[i].first);
        out += ':';
        write(j.o[i].second, out);
      }
      out += '}';
      break;
  }
}

inline std::string dump(const Json& j) {
  std::string s;
  write(j, s);
  return s;
}

}  // namespace json
