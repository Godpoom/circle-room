// Tokenizers for text search: SentencePiece-unigram (Marian th->en) and CLIP byte-level BPE.
// Tables come from ai\mt_vocab.bin / ai\clip_bpe.bin (tools/export_tokenizers.py).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai {

class UnigramTokenizer {
 public:
  bool load(const std::string& file);
  bool ok() const { return !pieces_.empty(); }
  std::vector<int64_t> encode(const std::string& utf8) const;  // ends with </s>
  std::string decode(const std::vector<int64_t>& ids) const;
  int eos() const { return eos_; }
  int pad() const { return pad_; }
  int vocabSize() const { return (int)pieces_.size(); }

 private:
  std::vector<std::string> pieces_;
  std::vector<float> scores_;
  std::unordered_map<std::string, int> index_;
  int unk_ = 0, eos_ = 0, pad_ = 0, maxLen_ = 16;
  float minScore_ = 0;
};

class ClipBpe {
 public:
  bool load(const std::string& file);
  bool ok() const { return !vocab_.empty(); }
  std::vector<int64_t> encode(const std::string& text, int ctx = 77) const;  // padded with <|endoftext|>

 private:
  std::vector<std::string> bpe(const std::string& word) const;
  std::unordered_map<std::string, int> vocab_;
  std::unordered_map<std::string, int> ranks_;  // "a b" -> merge rank
  std::string byteEnc_[256];                     // byte -> UTF-8 of its printable stand-in
};

}  // namespace ai
