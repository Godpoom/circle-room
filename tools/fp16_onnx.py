"""Converts an fp32 ONNX graph to fp16 (weights and I/O), halving its size.
Darkroom's ONNX wrapper converts fp16 inputs/outputs itself, so the app needs no change.

Usage: python fp16_onnx.py in_fp32.onnx out_fp16.onnx   (needs onnx + onnxruntime)
"""
import sys

import onnx
from onnxruntime.transformers.float16 import convert_float_to_float16

m = convert_float_to_float16(onnx.load(sys.argv[1]), keep_io_types=False)
onnx.save(m, sys.argv[2])
print("wrote", sys.argv[2])
