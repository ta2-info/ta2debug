#pragma once

// 横断値型 ta2base: メッセージ ID + 名前付き引数。設計書 design/REFACTORING.md §8.2。
// core / engine は表示文を作らず、この Msg を返す。翻訳（Translator / Catalog）は
// L3 だけが持つ（このライブラリには入れない）。ta2base は他のどの層にも依存しない。

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ta2 {

// メッセージ ID。定義の出所は Messages.def だけ。
enum class MsgId : uint16_t {
#define TA2_MSG(id, key, ja) id,
#include "base/Messages.def"
#undef TA2_MSG
  Count_
};

// ID の安定キー（Messages.def の第 2 引数。例 "err.target.task_for_pid"）。ログや AI 向けの
// エラー識別子に使う。表示文（第 3 引数）は ta2base には持たない（L3 の Catalog が読む）。
// 範囲外の ID には "" を返す。
const char* msgKey(MsgId id);

struct Msg;

// 名前付き引数 1 個。カタログ側の {name} に差し込まれる。
struct MsgArg {
  std::string name;                                   // "addr" "n" "name" "detail" ...
  std::variant<int64_t, uint64_t, std::string, std::shared_ptr<Msg>> value;
  bool hex = false;                                   // uint64_t を 0x.. で出すか
};

struct Msg {
  MsgId id = MsgId::None;
  std::vector<MsgArg> args;

  bool empty() const { return id == MsgId::None; }
  // 名前で引数を引く（無ければ nullptr。同名が複数あれば最初の 1 個）。Translator が {name} を
  // 差し込むときと、上位層が {detail} などを取り出すときに使う。
  const MsgArg* find(const char* name) const;

  static Msg of(MsgId id);
  // 引数を足して自分を返す（連鎖用）。一時オブジェクトにも続けて書けるよう & と && の両方を持つ。
  Msg& hex(const char* name, uint64_t v) &;
  Msg& num(const char* name, int64_t v) &;
  Msg& str(const char* name, std::string v) &;
  Msg& sub(const char* name, Msg m) &;                // 入れ子（式エラー + 文字位置など）
  Msg&& hex(const char* name, uint64_t v) && { return std::move(hex(name, v)); }
  Msg&& num(const char* name, int64_t v) && { return std::move(num(name, v)); }
  Msg&& str(const char* name, std::string v) && { return std::move(str(name, std::move(v))); }
  Msg&& sub(const char* name, Msg m) && { return std::move(sub(name, std::move(m))); }
};

} // namespace ta2
