#include "i18n.h"

#include <string>

static const char* kEn[] = {
#define I18N_EN(k, en, th) en,
    I18N_STRINGS(I18N_EN)
#undef I18N_EN
};
static const char* kTh[] = {
#define I18N_TH(k, en, th) th,
    I18N_STRINGS(I18N_TH)
#undef I18N_TH
};

static Lang g_lang = Lang::EN;

void setLang(Lang l) { g_lang = l; }
Lang currentLang() { return g_lang; }

const char* tr(int key) {
  if (key < 0 || key >= T_COUNT) return "";
  return g_lang == Lang::TH ? kTh[key] : kEn[key];
}

const char* trId(int key, const char* id) {
  // Small ring so several results can be alive within one ImGui call.
  static std::string ring[8];
  static int slot = 0;
  std::string& s = ring[slot = (slot + 1) & 7];
  s = tr(key);
  s += "###";
  s += id;
  return s.c_str();
}
