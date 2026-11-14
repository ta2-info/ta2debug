#pragma once

// L1 Core（ta2core）: gdb 風メモリ検査 `x/NFS` の解釈と値の取り出し。由来は v1 main.cpp の
// examineMemory / classicDump / formatValue / parseHexBytes（設計書 §4.2）。
// v1 は結果を表示行（g_output）とタイトル（g_outputTitle）へ直接書き、継続位置を g_xNextAddr に
// 置いていた。v2 では ExamineResult（行ごとのアドレス + 値 + 種別、継続位置）を返し、行の整形・
// タイトル・イメージ相対表示・シンボル注記（'a' 書式の describe、'i' の call 先名）は L3 が行う。
// 書式の既定値や 1 行あたりの個数など「解釈」の規則は v1 のまま。
#include <cstdint>
#include <string>
#include <vector>

#include "base/Msg.h"
#include "core/Breakpoints.h"
#include "core/Disassembler.h"
#include "platform/Target.h"

namespace ta2 {

// 書式指定 "[個数][書式][サイズ]"（例: 4xg / 8c / 10i / s）を解釈した結果（既定値を適用済み）。
struct ExamineSpec {
  int count = 8;    // 個数（省略時: c=32, s=4, その他 8）
  char fmt = 'x';   // x:16進 d:符号付 u:符号無 o:8進 t:2進 c:文字 a:アドレス f:浮動小数 i:命令 s:文字列
  int size = 8;     // 1/2/4/8（省略時 8。c は常に 1。f でサイズ < 4 なら 8）
};
// 解釈に失敗（不明な文字）したら false と err.examine.bad_format。
bool parseExamineSpec(const std::string& spec, ExamineSpec& out, Msg& err);

enum class ExamineKind {
  Values,   // 数値 / 文字（fmt = x d u o t c a f）
  Strings,  // null 終端文字列（fmt = s）
  Insns,    // 逆アセンブル（fmt = i）
  Classic,  // 従来の 16 進 + ASCII ダンプ（書式指定なしの :x）
};

struct ExamineValue {
  uint64_t value = 0;   // リトルエンディアンで size バイトを組んだ生値
  std::string text;     // formatExamineValue(value, fmt, size) の結果
};

struct ExamineLine {
  uint64_t addr = 0;                 // 行の先頭アドレス
  std::vector<ExamineValue> values;  // Values: その行の値（v1 と同じ個数で折り返し済み）
  std::string str;                   // Strings: 文字列（非表示文字は '.' に置換済み）
  Insn insn;                         // Insns: 1 命令（ブレイク命令は元命令に戻してある）
  std::vector<uint8_t> bytes;        // Classic: その行の生バイト（最大 16）
};

struct ExamineResult {
  ExamineKind kind = ExamineKind::Values;
  uint64_t addr = 0;      // 開始アドレス
  int count = 0;          // 個数（Values は実際に読めた数へ切り詰め済み。Insns は要求数）
  char fmt = 'x';
  int size = 0;           // Values の 1 個のバイト数
  size_t bytesRead = 0;   // Classic: 実際に読めたバイト数
  std::vector<ExamineLine> lines;
  uint64_t nextAddr = 0;  // 継続位置（Enter / 引数なし :x で続きを出す開始アドレス）
};

// 1 個の値を書式 fmt・サイズ size で文字列化（v1 formatValue）。数値表記だけで翻訳対象を含まない。
// 'a'（アドレス）は 16 進のみ返す（v1 はここで describe を連結していた。v2 ではシンボル注記は
// 表示側が resolveAddress で添える）。
std::string formatExamineValue(uint64_t v, char fmt, int size);

// HEX 文字列（空白無視）→ バイト列（setmem 用）。空・奇数長・不正文字で false。
bool parseHexBytes(const std::string& in, std::vector<uint8_t>& out);

class MemoryExaminer {
public:
  // intelSyntax: 'i' 書式の逆アセンブル記法（設定の持ち主は L2。記法切替の無いアーキでは無視）。
  MemoryExaminer(Target& target, const Breakpoints& bps, bool intelSyntax = false)
    : target_(target), bps_(bps), intel_(intelSyntax) {}

  // spec（"4xg" 等）で addr を検査する。書式不正・読取不可なら false と err。
  bool examine(const std::string& spec, uint64_t addr, ExamineResult& out, Msg& err);
  bool examine(const ExamineSpec& spec, uint64_t addr, ExamineResult& out, Msg& err);

  // 従来の 16 進 + ASCII ダンプ。bytes は 1..4096 に丸める。読めなければ false と err.mem.unreadable。
  bool classicDump(uint64_t addr, int bytes, ExamineResult& out, Msg& err);

private:
  Target& target_;
  const Breakpoints& bps_;
  bool intel_ = false;
};

} // namespace ta2
