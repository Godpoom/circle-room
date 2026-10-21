#include "history.h"

static const int kMaxEntries = 300;

void History::reset(const EditParams& p, int label) {
  e_.clear();
  e_.push_back({p, label, std::string()});
  idx_ = 0;
}

void History::push(const EditParams& p, int label, const std::string& extra) {
  if (idx_ >= 0 && sameParams(e_[idx_].p, p)) return;
  e_.resize(idx_ + 1);
  e_.push_back({p, label, extra});
  if ((int)e_.size() > kMaxEntries) e_.erase(e_.begin() + 1);  // keep the original state
  idx_ = (int)e_.size() - 1;
}

const EditParams& History::undo() {
  if (canUndo()) idx_--;
  return e_[idx_].p;
}

const EditParams& History::redo() {
  if (canRedo()) idx_++;
  return e_[idx_].p;
}

const EditParams& History::jump(int i) {
  if (i >= 0 && i < (int)e_.size()) idx_ = i;
  return e_[idx_].p;
}
