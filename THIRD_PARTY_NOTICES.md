# Third-party components

Circle Room bundles the components below. Each stays under its own license; the **full license
texts are in the [`LICENSES/`](LICENSES) folder**, which also ships inside every release zip.

## Libraries (in `third_party/`, compiled into `CircleRoom.exe` or shipped next to it)
| Component | License | Full text |
|---|---|---|
| [GLFW 3.4](https://www.glfw.org/) | zlib/libpng | `LICENSES/GLFW-zlib.md` |
| [Dear ImGui 1.91.8](https://github.com/ocornut/imgui) | MIT | `LICENSES/DearImGui-MIT.txt` |
| [stb_image / stb_image_write](https://github.com/nothings/stb) | MIT or public domain | `LICENSES/stb-MIT-or-PublicDomain.txt` |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) (`onnxruntime.dll`) | MIT, plus its own third-party notices | `LICENSES/ONNXRuntime-MIT.txt`, `LICENSES/ONNXRuntime-ThirdPartyNotices.txt` |
| [DirectML](https://www.nuget.org/packages/Microsoft.AI.DirectML) (`DirectML.dll`) | Microsoft DirectML license (redistribution in Windows apps allowed, section 1a) | `LICENSES/DirectML-*.txt` |
| Microsoft Visual C++ runtime DLLs (`ai/msvcp140*.dll`, `ai/vcruntime140*.dll`) | Microsoft Visual C++ Redistributable terms | see note below |

## AI models (folder `ai/` of the full release zip)
| File | Source | License | Full text |
|---|---|---|---|
| `clip_b32_vision_int8.onnx`, `clip_b32_text_int8.onnx`, `clip_bpe.bin` | OpenAI CLIP ViT-B/32 via [Xenova/clip-vit-base-patch32](https://huggingface.co/Xenova/clip-vit-base-patch32) | MIT | `LICENSES/CLIP-MIT.txt` |
| `aesthetic_b32.bin`, `prompts.bin` | [LAION aesthetic predictor](https://github.com/LAION-AI/aesthetic-predictor) (converted with `tools/convert_head.py`); prompt embeddings made with CLIP (`tools/make_prompts.py`) | MIT | `LICENSES/LAION-aesthetic-predictor-MIT.txt` |
| `mt_th_en_*.onnx`, `mt_vocab.bin` | [Helsinki-NLP/opus-mt-th-en](https://huggingface.co/Helsinki-NLP/opus-mt-th-en) via Xenova | Apache-2.0 | `LICENSES/opus-mt-th-en-Apache-2.0.txt` |
| `upernet_convnext_t_ade_fp16.onnx` | [openmmlab/upernet-convnext-tiny](https://huggingface.co/openmmlab/upernet-convnext-tiny) (UperNet + [ConvNeXt](https://github.com/facebookresearch/ConvNeXt), weights from [MMSegmentation](https://github.com/open-mmlab/mmsegmentation)), exported with `tools/export_upernet.py` + `tools/fp16_onnx.py` | MIT (model card); ConvNeXt MIT; MMSegmentation Apache-2.0 | `LICENSES/ConvNeXt-MIT.txt`, `LICENSES/mmsegmentation-Apache-2.0.txt` |
| `lama_inpaint.onnx` | [LaMa](https://github.com/advimman/lama) via [opencv/inpainting_lama](https://huggingface.co/opencv/inpainting_lama) (prepared with `tools/prepare_lama.py`) | Apache-2.0 | `LICENSES/LaMa-Apache-2.0.txt` |
| `yunet.onnx` | [OpenCV Zoo YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) | MIT | `LICENSES/YuNet-MIT.txt` |

## Notes
- **Region model changed in 1.0.1.** Version 1.0.0 shipped SegFormer-B3, whose NVIDIA Source Code
  License allows research and evaluation use only. It was removed and replaced by UperNet-ConvNeXt-T,
  which covers the same 150 ADE20K classes.
- **Training data.** The region model was trained on ADE20K (MIT Scene Parsing). The model weights are
  published under MIT; the images of the dataset itself carry their own terms and are not distributed here.
- **Visual C++ runtime.** These DLLs are needed by `onnxruntime.dll` on PCs without the
  [Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist).
  Microsoft permits app-local copies for licensees of Visual Studio (including the free Community edition).
- Fonts are not distributed: the app uses fonts already installed with Windows (Segoe UI, Leelawadee UI,
  Bahnschrift). The download website loads Barlow Condensed, IBM Plex Sans Thai and IBM Plex Mono from
  Google Fonts (SIL Open Font License).
- The sample photo on the website is synthetic (drawn by code), not a third-party photo.
