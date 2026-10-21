"""Exports the Marian (th->en) unigram vocab and the CLIP BPE tables as small binaries for C++,
plus reference token ids so the C++ tokenizers can be checked against HF `tokenizers`.

Usage: python export_tokenizers.py mt_tokenizer.json clip_tokenizer.json ai/ tests_out.txt
  ai/mt_vocab.bin : b"PEUV", u32 n, u32 unk, u32 eos, u32 pad, then n * (u16 len, utf8, f32 score)
  ai/clip_bpe.bin : b"PEBP", u32 nvocab, u32 nmerges, vocab: n * (u16 len, utf8) in id order,
                    merges: m * (u16 len, utf8 "a b") in rank order
"""
import json
import struct
import sys

from tokenizers import Tokenizer

sys.stdout.reconfigure(encoding="utf-8")


def wstr(f, s):
    b = s.encode("utf-8")
    f.write(struct.pack("<H", len(b)) + b)


def main(mt_path, clip_path, out_dir, tests_path):
    mt = json.load(open(mt_path, encoding="utf-8"))
    vocab = mt["model"]["vocab"]
    unk = mt["model"].get("unk_id") or 0
    specials = {t["content"]: t["id"] for t in mt.get("added_tokens", [])}
    eos, pad = specials.get("</s>", 0), specials.get("<pad>", 62306)
    with open(out_dir + "/mt_vocab.bin", "wb") as f:
        f.write(b"PEUV" + struct.pack("<IIII", len(vocab), unk, eos, pad))
        for piece, score in vocab:
            wstr(f, piece)
            f.write(struct.pack("<f", score))
    print("mt vocab", len(vocab), "unk", unk, "eos", eos, "pad", pad, "post", str(mt.get("post_processor"))[:100])

    cl = json.load(open(clip_path, encoding="utf-8"))
    cv = cl["model"]["vocab"]
    merges = cl["model"]["merges"]
    by_id = sorted(cv.items(), key=lambda kv: kv[1])
    assert [i for _, i in by_id] == list(range(len(by_id)))
    with open(out_dir + "/clip_bpe.bin", "wb") as f:
        f.write(b"PEBP" + struct.pack("<II", len(by_id), len(merges)))
        for tok, _ in by_id:
            wstr(f, tok)
        for m in merges:
            wstr(f, m if isinstance(m, str) else " ".join(m))
    print("clip vocab", len(by_id), "merges", len(merges))

    # Reference ids for the C++ tokenizer check
    mt["normalizer"] = {"type": "NFKC"}  # the Xenova file has an empty precompiled map
    mtok = Tokenizer.from_str(json.dumps(mt))
    ctok = Tokenizer.from_file(clip_path)
    th = ["ผีเสื้อ", "สุนัข", "คนใส่ชุดแตงโม", "คนกำลังวิ่ง ริมทะเล", "ภาพตอนขึ้นจากน้ำ", "ผู้ชายผูกเนคไท"]
    en = ["a photo of A butterfly.", "a photo of people running on the beach", "a photo of a man's tie, 2 dogs!"]
    with open(tests_path, "w", encoding="utf-8") as f:
        for s in th:
            f.write("mt\t" + s + "\t" + " ".join(map(str, mtok.encode(s).ids)) + "\n")
        for s in en:
            f.write("clip\t" + s + "\t" + " ".join(map(str, ctok.encode(s).ids)) + "\n")
    print("wrote", tests_path)


if __name__ == "__main__":
    main(*sys.argv[1:5])
