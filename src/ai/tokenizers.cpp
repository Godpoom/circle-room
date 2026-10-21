#include "ai/tokenizers.h"

#include <algorithm>
#include <climits>
#include <cstring>

#include "platform_win.h"

namespace ai {

namespace {
struct Reader {
  const std::vector<unsigned char>& b;
  size_t o = 0;
  bool ok = true;
  uint32_t u32() {
    uint32_t v = 0;
    if (o + 4 > b.size()) { ok = false; return 0; }
    std::memcpy(&v, &b[o], 4);
    o += 4;
    return v;
  }
  float f32() {
    float v = 0;
    if (o + 4 > b.size()) { ok = false; return 0; }
    std::memcpy(&v, &b[o], 4);
    o += 4;
    return v;
  }
  std::string str() {
    if (o + 2 > b.size()) { ok = false; return std::string(); }
    uint16_t n = (uint16_t)(b[o] | (b[o + 1] << 8));
    o += 2;
    if (o + n > b.size()) { ok = false; return std::string(); }
    std::string s((const char*)&b[o], n);
    o += n;
    return s;
  }
};

size_t utf8Len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

std::string cpToUtf8(uint32_t cp) {
  std::string s;
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
  else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
  return s;
}

const char* kMeta = "\xE2\x96\x81";  // U+2581, SentencePiece word boundary
}  // namespace

// ---------------------------------------------------------------- unigram
bool UnigramTokenizer::load(const std::string& file) {
  std::vector<unsigned char> b;
  if (!readFileBytes(file, b) || b.size() < 20 || std::memcmp(b.data(), "PEUV", 4) != 0) return false;
  Reader r{b, 4};
  uint32_t n = r.u32();
  unk_ = (int)r.u32();
  eos_ = (int)r.u32();
  pad_ = (int)r.u32();
  pieces_.resize(n);
  scores_.resize(n);
  minScore_ = 0;
  maxLen_ = 1;
  for (uint32_t i = 0; i < n && r.ok; i++) {
    pieces_[i] = r.str();
    scores_[i] = r.f32();
    minScore_ = std::min(minScore_, scores_[i]);
    index_.emplace(pieces_[i], (int)i);
    int cps = 0;
    for (size_t k = 0; k < pieces_[i].size(); k += utf8Len((unsigned char)pieces_[i][k])) cps++;
    maxLen_ = std::max(maxLen_, std::min(cps, 24));
  }
  if (!r.ok) pieces_.clear();
  return r.ok;
}

// NFKC (as SentencePiece's nmt_nfkc): Windows NormalizeString, else the Thai/Lao SARA AM split.
static std::string nfkc(const std::string& s) {
  typedef int(__stdcall * NormFn)(int, const wchar_t*, int, wchar_t*, int);
  static NormFn fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    for (const char* dll : {"kernel32.dll", "normaliz.dll"})
      if (!fn) fn = (NormFn)getProcAddressU(dll, "NormalizeString");
  }
  std::wstring w = utf8ToWide(s);
  if (fn && !w.empty()) {
    int n = fn(5 /*NormalizationKC*/, w.c_str(), (int)w.size(), nullptr, 0);
    if (n > 0) {
      std::wstring o((size_t)n, L'\0');
      n = fn(5, w.c_str(), (int)w.size(), &o[0], n);
      if (n > 0) return wideToUtf8(o.substr(0, (size_t)n));
    }
  }
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (i + 2 < s.size() && (unsigned char)s[i] == 0xE0 && (unsigned char)s[i + 1] == 0xB8 && (unsigned char)s[i + 2] == 0xB3) {
      o += "\xE0\xB9\x8D\xE0\xB8\xB2";  // U+0E33 -> U+0E4D U+0E32
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<int64_t> UnigramTokenizer::encode(const std::string& textIn) const {
  std::string text = nfkc(textIn);
  std::vector<int64_t> ids;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) i++;
    size_t j = i;
    while (j < text.size() && !(text[j] == ' ' || text[j] == '\t' || text[j] == '\n' || text[j] == '\r')) j++;
    if (j == i) break;
    std::string w = std::string(kMeta) + text.substr(i, j - i);
    i = j;
    // Viterbi over code points: best total score of a segmentation ending at each boundary.
    std::vector<size_t> off;
    for (size_t k = 0; k < w.size(); k += utf8Len((unsigned char)w[k])) off.push_back(k);
    off.push_back(w.size());
    size_t n = off.size() - 1;
    std::vector<double> best(n + 1, -1e30);
    std::vector<int> from(n + 1, -1), tok(n + 1, -1);
    best[0] = 0;
    for (size_t a = 0; a < n; a++) {
      if (best[a] <= -1e29) continue;
      bool single = false;
      for (size_t len = 1; len <= (size_t)maxLen_ && a + len <= n; len++) {
        auto it = index_.find(w.substr(off[a], off[a + len] - off[a]));
        if (it == index_.end()) continue;
        if (len == 1) single = true;
        double c = best[a] + scores_[it->second];
        if (c > best[a + len]) {
          best[a + len] = c;
          from[a + len] = (int)a;
          tok[a + len] = it->second;
        }
      }
      if (!single) {  // unknown character
        double c = best[a] + minScore_ - 10.0;
        if (c > best[a + 1]) {
          best[a + 1] = c;
          from[a + 1] = (int)a;
          tok[a + 1] = unk_;
        }
      }
    }
    std::vector<int64_t> word;
    for (int p = (int)n; p > 0; p = from[p]) {
      if (from[p] < 0) break;
      if (!(tok[p] == unk_ && !word.empty() && word.back() == unk_)) word.push_back(tok[p]);  // fuse unknowns
    }
    ids.insert(ids.end(), word.rbegin(), word.rend());
  }
  ids.push_back(eos_);
  return ids;
}

std::string UnigramTokenizer::decode(const std::vector<int64_t>& ids) const {
  std::string s;
  for (int64_t id : ids) {
    if (id < 0 || id >= (int64_t)pieces_.size() || id == eos_ || id == pad_ || id == unk_) continue;
    s += pieces_[(size_t)id];
  }
  std::string out;
  for (size_t k = 0; k < s.size();) {
    if (s.compare(k, 3, kMeta) == 0) {
      out += ' ';
      k += 3;
    } else {
      out += s[k++];
    }
  }
  size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
  return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

// ---------------------------------------------------------------- CLIP BPE
bool ClipBpe::load(const std::string& file) {
  std::vector<unsigned char> b;
  if (!readFileBytes(file, b) || b.size() < 12 || std::memcmp(b.data(), "PEBP", 4) != 0) return false;
  Reader r{b, 4};
  uint32_t nv = r.u32(), nm = r.u32();
  for (uint32_t i = 0; i < nv && r.ok; i++) vocab_.emplace(r.str(), (int)i);
  for (uint32_t i = 0; i < nm && r.ok; i++) ranks_.emplace(r.str(), (int)i);
  // GPT-2 byte <-> printable unicode table
  std::vector<int> bs, cs;
  for (int c = '!'; c <= '~'; c++) bs.push_back(c);
  for (int c = 0xA1; c <= 0xAC; c++) bs.push_back(c);
  for (int c = 0xAE; c <= 0xFF; c++) bs.push_back(c);
  cs = bs;
  int extra = 0;
  for (int c = 0; c < 256; c++)
    if (std::find(bs.begin(), bs.end(), c) == bs.end()) {
      bs.push_back(c);
      cs.push_back(256 + extra++);
    }
  for (size_t i = 0; i < bs.size(); i++) byteEnc_[bs[i]] = cpToUtf8((uint32_t)cs[i]);
  if (!r.ok) vocab_.clear();
  return r.ok;
}

std::vector<std::string> ClipBpe::bpe(const std::string& token) const {
  std::vector<std::string> sym;
  for (unsigned char c : token) sym.push_back(byteEnc_[c]);
  if (sym.empty()) return sym;
  sym.back() += "</w>";
  for (;;) {
    int bestRank = INT_MAX;
    size_t bestAt = 0;
    for (size_t k = 0; k + 1 < sym.size(); k++) {
      auto it = ranks_.find(sym[k] + " " + sym[k + 1]);
      if (it != ranks_.end() && it->second < bestRank) {
        bestRank = it->second;
        bestAt = k;
      }
    }
    if (bestRank == INT_MAX) break;
    std::string a = sym[bestAt], b = sym[bestAt + 1];
    std::vector<std::string> merged;
    for (size_t k = 0; k < sym.size(); k++) {
      if (k + 1 < sym.size() && sym[k] == a && sym[k + 1] == b) {
        merged.push_back(a + b);
        k++;
      } else {
        merged.push_back(sym[k]);
      }
    }
    sym.swap(merged);
  }
  return sym;
}

std::vector<int64_t> ClipBpe::encode(const std::string& textIn, int ctx) const {
  std::string text;
  for (char c : textIn) text += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  auto isLetter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || c >= 0x80; };
  auto isDigit = [](unsigned char c) { return c >= '0' && c <= '9'; };
  std::vector<int64_t> ids{49406};
  size_t i = 0;
  while (i < text.size()) {
    unsigned char c = (unsigned char)text[i];
    if (isSpace(c)) { i++; continue; }
    size_t j = i + 1;
    if (c == '\'') {  // contractions: 's 't 're 've 'm 'll 'd
      static const char* con[] = {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"};
      size_t hit = 0;
      for (const char* k : con)
        if (text.compare(i, strlen(k), k) == 0) hit = strlen(k);
      if (hit) j = i + hit;
      else while (j < text.size() && !isSpace((unsigned char)text[j]) && !isLetter((unsigned char)text[j]) && !isDigit((unsigned char)text[j])) j++;
    } else if (isLetter(c)) {
      while (j < text.size() && isLetter((unsigned char)text[j])) j++;
    } else if (!isDigit(c)) {
      while (j < text.size() && !isSpace((unsigned char)text[j]) && !isLetter((unsigned char)text[j]) &&
             !isDigit((unsigned char)text[j]))
        j++;
    }
    for (const std::string& s : bpe(text.substr(i, j - i))) {
      auto it = vocab_.find(s);
      if (it != vocab_.end()) ids.push_back(it->second);
    }
    i = j;
  }
  if ((int)ids.size() > ctx - 1) ids.resize(ctx - 1);
  ids.push_back(49407);
  while ((int)ids.size() < ctx) ids.push_back(49407);
  return ids;
}

}  // namespace ai
