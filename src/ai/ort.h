// ONNX Runtime, loaded from bin\ai\onnxruntime.dll only while AI features run.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct OrtSession;

namespace ai {

std::string aiDir();  // <exe dir>\ai
bool ortLoad(std::string& err);
void ortUnload();  // all Models must be released first
bool ortLoaded();

enum class DType { F32, F16, I64 };

struct Input {
  const char* name;
  DType type;  // F32 data is converted automatically if the model expects F16
  const void* data;
  std::vector<int64_t> shape;
};

struct Tensor {  // outputs are always returned as float
  std::vector<float> data;
  std::vector<int64_t> shape;
};

class Model {
 public:
  ~Model() { release(); }
  bool load(const std::string& path, bool preferGpu, std::string& err);
  void release();
  bool ok() const { return s_ != nullptr; }
  bool onGpu() const { return gpu_; }
  // One float input -> all outputs (copied out as float).
  bool run(const float* in, const std::vector<int64_t>& shape, std::vector<Tensor>& outs, std::string& err);
  // Several named inputs; `wanted` selects outputs by name (empty = all).
  bool runEx(const std::vector<Input>& ins, const std::vector<std::string>& wanted, std::vector<Tensor>& outs,
             std::string& err);
  const std::vector<std::string>& outputNames() const { return outNames_; }

 private:
  OrtSession* s_ = nullptr;
  bool gpu_ = false;
  std::vector<std::string> inNames_, outNames_;
  std::vector<DType> inTypes_;
};

}  // namespace ai
