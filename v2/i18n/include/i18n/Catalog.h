#pragma once

// ta2i18n: メッセージカタログ。MsgId → テンプレート文字列（{name} プレースホルダ入り）の表。
// 設計書 design/REFACTORING.md §8.3。既定（日本語）は Messages.def の第 3 引数で、全 MsgId に
// 必ず入る。追加言語や試作は `キー = 文` の行形式テキスト（.msg）を overlay で重ねる。翻訳は
// この表を引いて差し込むだけ（Translator）。このライブラリは ta2base にしか依存しない。
#include <set>
#include <string>
#include <vector>

#include "base/Msg.h"

namespace ta2 {

// キー（Messages.def の第 2 引数）から MsgId を引く。未知キーは false。None（空キー）は対象外。
bool msgIdForKey(const std::string& key, MsgId& out);

// テンプレート中の {name} / {name:spec} のプレースホルダ名を集める（{{ }} は除く）。検査・照合用。
std::set<std::string> placeholderNames(const std::string& tmpl);

class Catalog {
public:
  // 既定（Messages.def の日本語）で全 MsgId を初期化する。
  Catalog();

  // overlay の結果（セルフテスト・起動時診断用）。
  struct Report {
    int applied = 0;                              // 適用した行数
    std::vector<std::string> unknownKeys;         // Messages.def に無いキー
    std::vector<std::string> placeholderMismatch; // 既定とプレースホルダ集合が食い違うキー
  };
  // `キー = 文` 形式の .msg テキストを重ねる（既存言語より優先）。`#` 始まりはコメント、空行は無視。
  // 値の \n \t \\ はエスケープとして解釈する（{{ }} は差し込み時に処理）。
  Report overlay(const std::string& msgText);

  // MsgId のテンプレート。overlay されていればそれ、無ければ既定日本語（常に入っている）。
  // 範囲外 ID は空文字。
  const std::string& templateFor(MsgId id) const;

private:
  std::vector<std::string> tmpl_; // MsgId 添字
  std::string empty_;
};

} // namespace ta2
