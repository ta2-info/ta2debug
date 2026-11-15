#include "i18n/Translator.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

// ロケール用カタログの組み立て（§8.3）。埋め込みカタログ（言語別）＋ユーザ上書きファイルを
// 既定日本語に重ねる。埋め込みは CMake が res/i18n/<lang>.msg から生成する文字列リテラル。

namespace ta2 {

extern const char kEnMsg[]; // EmbeddedEn.cpp（CMake 生成）

namespace {
// 言語に対応する埋め込みカタログ（.msg テキスト）。無ければ nullptr。いまは en のみ。
const char* embeddedCatalog(const std::string& lang) {
  if (lang == "en") return kEnMsg;
  return nullptr;
}
// ユーザ上書きの .msg を置くディレクトリ（§8.3）。TA2DEBUG_I18N_DIR 優先、無ければ
// ~/Library/Application Support/ta2debug/i18n（注釈と同じ親）。取れなければ空。
std::string userI18nDir() {
  if (const char* d = std::getenv("TA2DEBUG_I18N_DIR")) return d;
  if (const char* h = std::getenv("HOME")) return std::string(h) + "/Library/Application Support/ta2debug/i18n";
  return std::string();
}
} // namespace

Catalog loadCatalog(const std::string& lang, Catalog::Report* rep) {
  Catalog cat; // 既定日本語（全キーに必ず入っている）
  if (const char* emb = embeddedCatalog(lang)) {
    Catalog::Report r = cat.overlay(emb);
    if (rep) *rep = r;
  }
  // ユーザ上書き（選択言語）。あれば埋め込みより優先。試作や未収録言語に使える。
  std::string dir = userI18nDir();
  if (!dir.empty()) {
    std::ifstream f(dir + "/" + lang + ".msg");
    if (f) {
      std::stringstream ss; ss << f.rdbuf();
      Catalog::Report r = cat.overlay(ss.str());
      if (rep) *rep = r;
    }
  }
  return cat;
}

} // namespace ta2
