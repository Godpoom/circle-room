#pragma once
#include <string>
#include <vector>

#include "params.h"

// Undo/redo of parameter snapshots (~1.3 KB each), never pixels.
class History {
 public:
  struct Entry {
    EditParams p;
    int label;          // i18n key
    std::string extra;  // e.g. preset name
  };

  void reset(const EditParams& p, int label);
  void push(const EditParams& p, int label, const std::string& extra = std::string());
  bool canUndo() const { return idx_ > 0; }
  bool canRedo() const { return idx_ + 1 < (int)e_.size(); }
  const EditParams& undo();
  const EditParams& redo();
  const EditParams& jump(int i);
  int index() const { return idx_; }
  const std::vector<Entry>& entries() const { return e_; }

 private:
  std::vector<Entry> e_;
  int idx_ = -1;
};
