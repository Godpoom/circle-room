"""Prepares OpenCV Zoo's LaMa (inpainting_lama_2025jan.onnx) for fast loading in the app.

The published graph has ~18k nodes of dynamic-shape plumbing, so creating a session takes
~15 s. Pinning batch=1 and saving ORT's optimised graph brings that to ~0.3 s with the same
output (max |diff| ~0.001 on a 0..255 scale).

Usage: python prepare_lama.py inpainting_lama_2025jan.onnx ai/lama_inpaint.onnx
"""
import os
import subprocess
import sys
import tempfile

import onnxruntime as ort


def main(src, dst):
    fixed = os.path.join(tempfile.gettempdir(), "lama_fixed.onnx")
    subprocess.check_call([sys.executable, "-m", "onnxruntime.tools.make_dynamic_shape_fixed",
                           "--dim_param", "batch", "--dim_value", "1", src, fixed])
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_EXTENDED
    so.optimized_model_filepath = dst
    ort.InferenceSession(fixed, so, providers=["CPUExecutionProvider"])
    os.remove(fixed)
    print("wrote", dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
