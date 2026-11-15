#pragma once

// ta2i18n: Translator。Msg（ID + 名前付き引数）をカタログで翻訳して 1 本の文字列にする。
// 設計書 design/REFACTORING.md §8.4。プレースホルダ {name} を引数の型（hex/数値/文字列/入れ子 Msg）
// に応じて差し込む。書式は必要なものだけ: {addr:016x}（ゼロ詰め 16 進）/ {n:d}（10 進）/ {name:q}
// （引用符で囲む）。桁揃え（全角対応）は呼び出し側（UI の fitWidth）がやる。翻訳は描画のたびに行える
// ので、言語を差し替えても即反映される。
#include <string>

#include "base/Msg.h"
#include "i18n/Catalog.h"

namespace ta2 {

class Translator {
public:
  // カタログと言語名（表示・診断用。"ja" / "en" など）を束ねる。
  explicit Translator(Catalog cat, std::string lang = "ja")
    : cat_(std::move(cat)), lang_(std::move(lang)) {}

  // Msg を現在のカタログで翻訳する。空 Msg（id=None）は空文字。入れ子 Msg は再帰的に翻訳する。
  std::string tr(const Msg& m) const;

  const std::string& lang() const { return lang_; }
  const Catalog& catalog() const { return cat_; }

private:
  // テンプレートの {name}/{name:spec} を m の引数で差し込む（{{ }} は '{' '}' のリテラル）。
  std::string subst(const std::string& tmpl, const Msg& m) const;
  // 1 個の引数を spec（空/"016x"/"d"/"q" 等）で整形する。
  std::string formatArg(const MsgArg& a, const std::string& spec) const;

  Catalog cat_;
  std::string lang_;
};

// 環境変数からロケールの言語部を決める（LC_ALL → LC_MESSAGES → LANG、既定 "ja"）。§8.6。
// 例: "ja_JP.UTF-8" → "ja"、"en_US.UTF-8" → "en"、"C"/"POSIX"/未設定 → "ja"。
std::string detectLocaleLang();

} // namespace ta2
