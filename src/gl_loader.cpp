#include "gl_loader.h"

namespace glp {
#define GLP_DEF(ret, name, args) PFN_##name name = nullptr;
GLP_FUNCS(GLP_DEF)
#undef GLP_DEF
}  // namespace glp

PFN_glTexStorage2D glpTexStorage2D = nullptr;

const char* glpLoad(GlpGetProc getProc) {
  glpTexStorage2D = (PFN_glTexStorage2D)getProc("glTexStorage2D");
#define GLP_LOAD(ret, name, args)                       \
  glp::name = (glp::PFN_##name)getProc(#name);          \
  if (!glp::name) return #name;
  GLP_FUNCS(GLP_LOAD)
#undef GLP_LOAD
  return nullptr;
}
