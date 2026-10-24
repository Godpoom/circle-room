#include "ai/ort.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>

#include "onnxruntime_c_api.h"
#include "platform_win.h"

namespace ai {

// Only the first entry of OrtDmlApi is needed; avoids pulling in d3d12/DirectML headers.
struct DmlApiHead {
  OrtStatusPtr(ORT_API_CALL* SessionOptionsAppendExecutionProvider_DML)(OrtSessionOptions* options, int device_id);
};

static HMODULE g_dll = nullptr;
static const OrtApi* g_api = nullptr;
static OrtEnv* g_env = nullptr;
static std::mutex g_mutex;
static std::atomic<int> g_live{0};  // loaded sessions; the runtime stays while any exist

std::string aiDir() { return exeDir() + "\\ai"; }

static bool check(OrtStatus* st, std::string& err) {
  if (!st) return true;
  err = g_api->GetErrorMessage(st);
  g_api->ReleaseStatus(st);
  return false;
}

static void ignoreStatus(OrtStatus* st) {
  if (st) g_api->ReleaseStatus(st);
}

bool ortLoaded() { return g_api != nullptr; }

static bool ortLoadLocked(std::string& err) {
  if (g_api) return true;
  std::wstring dll = utf8ToWide(aiDir() + "\\onnxruntime.dll");
  // Its own imports (VC++ runtime, DirectML) come only from ai\ itself or System32: never from
  // the current folder or PATH, where someone else's DLL could be waiting.
  g_dll = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!g_dll) {
    err = "cannot load ai\\onnxruntime.dll (missing file or Visual C++ runtime)";
    return false;
  }
  typedef const OrtApiBase*(ORT_API_CALL * GetBase)();
  GetBase gb = (GetBase)(void*)GetProcAddress(g_dll, "OrtGetApiBase");
  const OrtApi* api = gb ? gb()->GetApi(ORT_API_VERSION) : nullptr;
  if (!api) {
    FreeLibrary(g_dll);
    g_dll = nullptr;
    err = "onnxruntime.dll is too old";
    return false;
  }
  g_api = api;
  if (!check(g_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "photoeditor", &g_env), err)) {
    g_api = nullptr;
    FreeLibrary(g_dll);
    g_dll = nullptr;
    return false;
  }
  return true;
}

bool ortLoad(std::string& err) {
  std::lock_guard<std::mutex> lk(g_mutex);
  return ortLoadLocked(err);
}

void ortUnload() {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!g_api || g_live > 0) return;  // another job still uses it
  if (g_env) g_api->ReleaseEnv(g_env);
  g_env = nullptr;
  g_api = nullptr;
  FreeLibrary(g_dll);  // gives the runtime's memory back to the OS
  g_dll = nullptr;
  trimWorkingSet();
}

void Model::release() {
  if (s_ && g_api) {
    g_api->ReleaseSession(s_);
    g_live--;
  }
  s_ = nullptr;
  outNames_.clear();
  inNames_.clear();
  inTypes_.clear();
}

bool Model::load(const std::string& path, bool preferGpu, std::string& err) {
  release();
  {
    // Reserve a slot while holding the lock so a concurrent ortUnload() cannot free the runtime.
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!ortLoadLocked(err)) return false;
    g_live++;
  }
  bool loaded = false;
  struct Guard {
    bool& ok;
    ~Guard() {
      if (!ok) g_live--;
    }
  } guard{loaded};
  for (int attempt = preferGpu ? 0 : 1; attempt < 2; attempt++) {
    bool gpu = attempt == 0;
    OrtSessionOptions* so = nullptr;
    if (!check(g_api->CreateSessionOptions(&so), err)) return false;
    ignoreStatus(g_api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
    int threads = (int)std::max(1u, std::thread::hardware_concurrency() / 2);
    ignoreStatus(g_api->SetIntraOpNumThreads(so, threads));
    // No arena: freed activations go back to the OS instead of being kept for the next run.
    ignoreStatus(g_api->DisableCpuMemArena(so));
    bool ok = true;
    if (gpu) {
      ignoreStatus(g_api->DisableMemPattern(so));
      ignoreStatus(g_api->SetSessionExecutionMode(so, ORT_SEQUENTIAL));
      const DmlApiHead* dml = nullptr;
      OrtStatus* st = g_api->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dml);
      ok = check(st, err) && dml && check(dml->SessionOptionsAppendExecutionProvider_DML(so, 0), err);
    }
    if (ok) ok = check(g_api->CreateSession(g_env, utf8ToWide(path).c_str(), so, &s_), err);
    g_api->ReleaseSessionOptions(so);
    if (!ok) {
      s_ = nullptr;
      continue;  // GPU failed -> retry on CPU
    }
    gpu_ = gpu;
    loaded = true;
    OrtAllocator* alloc = nullptr;
    ignoreStatus(g_api->GetAllocatorWithDefaultOptions(&alloc));
    char* name = nullptr;
    size_t nIn = 0;
    ignoreStatus(g_api->SessionGetInputCount(s_, &nIn));
    inNames_.clear();
    inTypes_.clear();
    for (size_t i = 0; i < nIn; i++) {
      if (!check(g_api->SessionGetInputName(s_, i, alloc, &name), err)) continue;
      inNames_.push_back(name);
      ignoreStatus(g_api->AllocatorFree(alloc, name));
      OrtTypeInfo* ti = nullptr;
      const OrtTensorTypeAndShapeInfo* tsi = nullptr;
      ONNXTensorElementDataType et = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
      if (!g_api->SessionGetInputTypeInfo(s_, i, &ti) && ti) {
        if (!g_api->CastTypeInfoToTensorInfo(ti, &tsi) && tsi) ignoreStatus(g_api->GetTensorElementType(tsi, &et));
        g_api->ReleaseTypeInfo(ti);
      }
      inTypes_.push_back(et == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ? DType::F16
                         : et == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ? DType::I64 : DType::F32);
    }
    size_t nOut = 0;
    ignoreStatus(g_api->SessionGetOutputCount(s_, &nOut));
    for (size_t i = 0; i < nOut; i++) {
      if (check(g_api->SessionGetOutputName(s_, i, alloc, &name), err)) {
        outNames_.push_back(name);
        ignoreStatus(g_api->AllocatorFree(alloc, name));
      }
    }
    err.clear();
    return true;
  }
  return false;
}

static uint16_t toHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000, mant = x & 0x7fffff;
  int exp = (int)((x >> 23) & 0xff) - 127 + 15;
  if (exp <= 0) return (uint16_t)sign;                       // flush tiny values to zero
  if (exp >= 31) return (uint16_t)(sign | 0x7c00);           // overflow -> inf
  uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
  if (mant & 0x1000) h++;                                    // round to nearest
  return (uint16_t)h;
}

static float fromHalf(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
  if (exp == 0) {
    if (!mant) x = sign;
    else {  // subnormal
      int e = -1;
      do { e++; mant <<= 1; } while (!(mant & 0x400));
      x = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mant & 0x3ff) << 13);
    }
  } else if (exp == 31) {
    x = sign | 0x7f800000 | (mant << 13);
  } else {
    x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}

bool Model::run(const float* in, const std::vector<int64_t>& shape, std::vector<Tensor>& outs, std::string& err) {
  if (inNames_.empty()) {
    err = "model not loaded";
    return false;
  }
  return runEx({{inNames_[0].c_str(), DType::F32, in, shape}}, {}, outs, err);
}

bool Model::runEx(const std::vector<Input>& ins, const std::vector<std::string>& wanted, std::vector<Tensor>& outs,
                  std::string& err) {
  if (!s_) {
    err = "model not loaded";
    return false;
  }
  OrtMemoryInfo* mi = nullptr;
  if (!check(g_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi), err)) return false;
  std::vector<OrtValue*> values;
  std::vector<const char*> inNames;
  std::vector<std::vector<uint16_t>> halves;  // keeps converted fp16 buffers alive
  bool ok = true;
  for (const Input& in : ins) {
    size_t count = 1;
    for (int64_t d : in.shape) count *= (size_t)d;
    DType want = in.type;
    for (size_t k = 0; k < inNames_.size(); k++)
      if (inNames_[k] == in.name && in.type == DType::F32 && inTypes_[k] == DType::F16) want = DType::F16;
    const void* data = in.data;
    size_t bytes;
    ONNXTensorElementDataType et;
    if (want == DType::F16) {
      halves.emplace_back(count);
      const float* f = (const float*)in.data;
      for (size_t i = 0; i < count; i++) halves.back()[i] = toHalf(f[i]);
      data = halves.back().data();
      bytes = count * 2;
      et = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    } else if (want == DType::I64) {
      bytes = count * 8;
      et = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    } else {
      bytes = count * 4;
      et = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    }
    OrtValue* v = nullptr;
    ok = check(g_api->CreateTensorWithDataAsOrtValue(mi, (void*)data, bytes, in.shape.data(), in.shape.size(), et, &v), err);
    if (!ok) break;
    values.push_back(v);
    inNames.push_back(in.name);
  }
  g_api->ReleaseMemoryInfo(mi);
  std::vector<const char*> names;
  if (wanted.empty())
    for (auto& n : outNames_) names.push_back(n.c_str());
  else
    for (auto& n : wanted) names.push_back(n.c_str());
  std::vector<OrtValue*> res(names.size(), nullptr);
  if (ok)
    ok = check(g_api->Run(s_, nullptr, inNames.data(), (const OrtValue* const*)values.data(), values.size(), names.data(),
                          names.size(), res.data()),
               err);
  for (OrtValue* v : values) g_api->ReleaseValue(v);
  outs.assign(names.size(), Tensor());
  for (size_t i = 0; i < res.size(); i++) {
    if (!res[i]) continue;
    if (ok) {
      OrtTensorTypeAndShapeInfo* info = nullptr;
      ignoreStatus(g_api->GetTensorTypeAndShape(res[i], &info));
      size_t nd = 0, elems = 0;
      ONNXTensorElementDataType et = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
      ignoreStatus(g_api->GetDimensionsCount(info, &nd));
      outs[i].shape.resize(nd);
      ignoreStatus(g_api->GetDimensions(info, outs[i].shape.data(), nd));
      ignoreStatus(g_api->GetTensorShapeElementCount(info, &elems));
      ignoreStatus(g_api->GetTensorElementType(info, &et));
      g_api->ReleaseTensorTypeAndShapeInfo(info);
      void* p = nullptr;
      ignoreStatus(g_api->GetTensorMutableData(res[i], &p));
      outs[i].data.resize(elems);
      if (et == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
        for (size_t k = 0; k < elems; k++) outs[i].data[k] = fromHalf(((const uint16_t*)p)[k]);
      else if (et == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
        for (size_t k = 0; k < elems; k++) outs[i].data[k] = (float)((const int64_t*)p)[k];
      else
        std::memcpy(outs[i].data.data(), p, elems * sizeof(float));
    }
    g_api->ReleaseValue(res[i]);
  }
  return ok;
}

}  // namespace ai
