"""Precomputes CLIP ViT-B/32 text embeddings for the zero-shot quality prompts.

Usage: python make_prompts.py text_model_quantized.onnx tokenizer.json ai/prompts.bin
The app never runs the text encoder; it only reads these vectors.
Format: b"PEPR", u32 count, u32 dim, then count * (char name[32], float32[dim]) (L2-normalised).
The order must match PromptId in src/ai/clip_model.h.
"""
import struct
import sys

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer

# (name, positive text, negative text)
PROMPTS = [
    ("composition", "a well composed photo with balanced framing",
     "a poorly composed photo with awkward framing"),
    ("subject", "a photo with a clear main subject", "a cluttered photo with no clear subject"),
    ("lighting", "a photo with beautiful dramatic lighting", "a photo with flat dull lighting"),
    ("sharpness", "a sharp photo in perfect focus", "a blurry out of focus photo"),
    ("exposure", "a well exposed photo", "a badly underexposed or overexposed photo"),
    ("level", "a photo with a straight level horizon", "a crooked tilted photo"),
    ("quality", "a stunning professional photograph", "a boring amateur snapshot"),
    ("eyes", "a close-up of a face with open eyes", "a close-up of a face with closed eyes"),
]

# Single prompts for scene detection (auto edit); order = Scene enum in src/ai/auto_edit.h.
SCENES = [
    ("portrait", "a portrait photo of a person"),
    ("group", "a group photo of people"),
    ("landscape", "a landscape photo of nature"),
    ("sunset", "a photo of a sunset or sunrise"),
    ("night", "a photo taken at night"),
    ("indoor", "a photo of an indoor room"),
    ("food", "a photo of food"),
    ("beach", "a photo of a beach"),
    ("snow", "a photo of snow"),
    ("forest", "a photo of a green forest"),
    ("city", "a photo of a city street or a building"),
    ("animal", "a photo of an animal"),
]


def main(model, tok_path, dst):
    tok = Tokenizer.from_file(tok_path)
    sess = ort.InferenceSession(model, providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0].name
    outs = [o.name for o in sess.get_outputs()]
    out_name = "text_embeds" if "text_embeds" in outs else outs[0]
    texts, names = [], []
    for name, pos, neg in PROMPTS:
        texts += [pos, neg]
        names += [name + "+", name + "-"]
    for name, text in SCENES:
        texts.append(text)
        names.append("scene:" + name)
    ids = []
    for t in texts:
        e = tok.encode(t).ids[:77]
        ids.append(e + [49407] * (77 - len(e)))  # pad with <|endoftext|>; pooling uses the first EOS
    emb = sess.run([out_name], {inp: np.array(ids, dtype=np.int64)})[0]
    emb = emb / np.linalg.norm(emb, axis=1, keepdims=True)
    with open(dst, "wb") as f:
        f.write(b"PEPR" + struct.pack("<II", len(texts), emb.shape[1]))
        for i, t in enumerate(texts):
            name = names[i].encode()[:31]
            f.write(name.ljust(32, b"\0"))
            f.write(emb[i].astype("<f4").tobytes())
    print("inputs", [i.name for i in sess.get_inputs()], "outputs", outs, "->", emb.shape, "wrote", dst)


if __name__ == "__main__":
    main(*sys.argv[1:4])
