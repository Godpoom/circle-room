# Third-party components

Darkroom bundles or downloads the components below. Each stays under its own license.

## Libraries (in `third_party/`)
| Component | License |
|---|---|
| [GLFW 3.4](https://www.glfw.org/) | zlib/libpng (`third_party/glfw/LICENSE.md`) |
| [Dear ImGui 1.91.8](https://github.com/ocornut/imgui) | MIT (`third_party/imgui/LICENSE.txt`) |
| [stb_image / stb_image_write](https://github.com/nothings/stb) | MIT or public domain (see file headers) |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) headers + DLL | MIT |
| DirectML.dll | Microsoft DirectML redistributable license |
| Microsoft Visual C++ runtime DLLs (Release zip only) | Microsoft Visual C++ Redistributable license |

## AI models (in the Release zip, folder `ai/`)
| File | Source | License |
|---|---|---|
| `clip_b32_vision_int8.onnx`, `clip_b32_text_int8.onnx`, `clip_bpe.bin` | OpenAI CLIP ViT-B/32 via [Xenova/clip-vit-base-patch32](https://huggingface.co/Xenova/clip-vit-base-patch32) | MIT |
| `aesthetic_b32.bin` | [LAION aesthetic predictor](https://github.com/LAION-AI/aesthetic-predictor) (converted with `tools/convert_head.py`) | MIT |
| `mt_th_en_*.onnx`, `mt_vocab.bin` | [Helsinki-NLP/opus-mt-th-en](https://huggingface.co/Helsinki-NLP/opus-mt-th-en) via Xenova | Apache-2.0 |
| `segformer_b3_ade_fp16.onnx` | [nvidia/segformer-b3-finetuned-ade-512-512](https://huggingface.co/nvidia/segformer-b3-finetuned-ade-512-512) via onnx-community | **NVIDIA Source Code License — non-commercial use only** |
| `lama_inpaint.onnx` | [LaMa](https://github.com/advimman/lama) via [opencv/inpainting_lama](https://huggingface.co/opencv/inpainting_lama) (prepared with `tools/prepare_lama.py`) | Apache-2.0 |
| `yunet.onnx` | [OpenCV Zoo YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) | MIT |

**Note on SegFormer:** its license allows research and non-commercial use only. The "Local adjustments"
and region-aware highlight recovery features depend on it. A commercial build must replace this model
with one under a permissive license.
