"""Exports an ADE20K UperNet-ConvNeXt (MIT, openmmlab on Hugging Face) to ONNX for Darkroom's
region masks. It replaces SegFormer, whose licence allows research/evaluation use only.

The graph returns the decode head's logits at 1/4 of the input size, [1, 150, H/4, W/4] like
SegFormer, so the app keeps its memory use and its post-processing unchanged. H and W are dynamic
(multiples of 32). The fp32 graph is written here; tools/fp16_onnx.py halves it afterwards.

Needs torch, transformers, onnx and onnxscript.
Usage: python export_upernet.py <hf-model-id> <out.onnx>
   e.g. python export_upernet.py openmmlab/upernet-convnext-small ai/upernet_convnext_s_fp32.onnx
"""
import sys

import torch
from transformers import UperNetForSemanticSegmentation


class AdaptiveAvgPoolAnySize(torch.nn.Module):
    """adaptive_avg_pool2d(x, b) written with an integral image, so the exported graph works for
    every input size (the stock op gets frozen to the size used while exporting)."""

    def __init__(self, bins):
        super().__init__()
        self.b = int(bins if isinstance(bins, int) else bins[0])

    def forward(self, x):
        b = self.b
        H, W = x.shape[2], x.shape[3]
        ii = torch.nn.functional.pad(x.cumsum(2).cumsum(3), (1, 0, 1, 0))
        i = torch.arange(b, device=x.device)
        h0, h1 = (i * H) // b, ((i + 1) * H + b - 1) // b  # same cell edges as PyTorch
        w0, w1 = (i * W) // b, ((i + 1) * W + b - 1) // b
        def corner(hs, ws):
            return ii.index_select(2, hs).index_select(3, ws)
        s = corner(h1, w1) - corner(h0, w1) - corner(h1, w0) + corner(h0, w0)
        area = ((h1 - h0)[:, None] * (w1 - w0)[None, :]).to(x.dtype)
        return s / area


def swap_pools(m):
    n = 0
    for mod in list(m.modules()):
        for cname, child in list(mod.named_children()):
            if isinstance(child, torch.nn.AdaptiveAvgPool2d):
                new = AdaptiveAvgPoolAnySize(child.output_size)
                setattr(mod, cname, new)
                # UperNet's pyramid blocks also call their layers through a plain Python list
                layers = getattr(mod, "layers", None)
                if isinstance(layers, list):
                    mod.layers = [new if l is child else l for l in layers]
                n += 1
    assert n > 0, "no adaptive pooling found"
    return n


class QuarterLogits(torch.nn.Module):
    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, pixel_values):
        feats = self.m.backbone.forward_with_filtered_kwargs(pixel_values).feature_maps
        return self.m.decode_head(feats)  # [1, 150, H/4, W/4]: skip the full-size upsample


def main():
    model_id, out = sys.argv[1], sys.argv[2]
    m = UperNetForSemanticSegmentation.from_pretrained(model_id, use_safetensors=None).eval()
    w = QuarterLogits(m).eval()
    x = torch.randn(1, 3, 480, 640)
    with torch.no_grad():
        ref = w(x)
        print("swapped pools:", swap_pools(m))
        for shape in [(1, 3, 480, 640), (1, 3, 640, 448), (1, 3, 352, 640)]:
            t = torch.randn(*shape)
            m2 = UperNetForSemanticSegmentation.from_pretrained(model_id).eval()
            diff = (QuarterLogits(m2)(t) - w(t)).abs().max().item()
            print("pool swap check", shape, "max |diff|", f"{diff:.2e}")
    print("logits", tuple(ref.shape))
    dyn = torch.export.Dim.DYNAMIC
    prog = torch.onnx.export(w, (x,), input_names=["pixel_values"], output_names=["logits"], opset_version=18,
                             dynamic_shapes={"pixel_values": {2: dyn, 3: dyn}}, dynamo=True, external_data=False)
    prog.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
