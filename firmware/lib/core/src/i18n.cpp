#include "i18n.h"

#include <stddef.h>

namespace {

#define T5_STRING_EN(id, en, nl) en,
#define T5_STRING_NL(id, en, nl) nl,
const char *const kEn[] = {T5_STRINGS(T5_STRING_EN)};
const char *const kNl[] = {T5_STRINGS(T5_STRING_NL)};
#undef T5_STRING_EN
#undef T5_STRING_NL
static_assert(sizeof(kEn) / sizeof(kEn[0]) == size_t(S::Count), "English table");
static_assert(sizeof(kNl) / sizeof(kNl[0]) == size_t(S::Count), "Dutch table");

Lang current = Lang::En;

}  // namespace

void setLanguage(Lang lang) { current = lang < Lang::Count ? lang : Lang::En; }
Lang language() { return current; }

const char *tr(S id, Lang lang) {
  if (id >= S::Count) return "?";
  return (lang == Lang::Nl ? kNl : kEn)[size_t(id)];
}

const char *tr(S id) { return tr(id, current); }

const char *languageName(Lang lang) { return lang == Lang::Nl ? "Nederlands" : "English"; }
