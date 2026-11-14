#pragma once

// L1 Core（ta2core）: アドレス → 「イメージ + オフセット + シンボル + メモ」の構造化結果。
// 由来は v1 Session::describe / Session::symbolName（文字列を直接組み立てていた）。
// v2 では算出（resolveAddress）と整形（format*）を分ける。整形の中身はシンボル名・イメージ名・
// 16 進だけで翻訳対象の文を含まない（"symbol stub for:" 等は v1 から固定の英語表記）ので、
// v1 と同じ文字列を作る補助を L1 に置いてよい（L3 の TextReport はこれを使っても、SymbolRef から
// 独自に組んでもよい）。
#include <cstdint>
#include <string>

#include "core/Annotations.h"
#include "core/ImageList.h"
#include "core/Symbols.h"

namespace ta2 {

struct SymbolRef {
  uint64_t addr = 0;
  bool hasImage = false;        // どれかのイメージに属するか（false なら以下すべて無効）
  std::string image;            // イメージ名（ファイル名部分）
  uint64_t imageOffset = 0;     // addr - loadAddr
  bool inStub = false;          // __stubs（シンボルスタブ）内か
  std::string stubTarget;       // inStub のとき指す先のシンボル名（未解決なら空）
  bool hasSymbol = false;       // addr 以下で最も近いシンボルがあるか（inStub のときは引かない）
  std::string symbol;           // 表示名（デマングル済み）
  uint64_t symbolOffset = 0;    // addr - シンボル先頭
  std::string memo;             // addr に完全一致するユーザ注釈のメモ（無ければ空）
};

// addr を解決する。annots は nullptr 可（メモを引かない）。displayName を使うので、v1 と同じく
// 「状態を変えるスレッド」（v1 のメインスレッド、v2 のエンジンスレッド）から呼ぶこと。
// wantMemo=false ならメモを引かない（v1 symbolName 相当の軽い経路）。
SymbolRef resolveAddress(ImageList& images, Symbols& symbols, Annotations* annots,
                         uint64_t addr, bool wantMemo = true);

// v1 Session::describe と同じ文字列: "image 0x.. func+0xNN"（+ "  ; メモ"）。
// スタブは "image 0x..  symbol stub for: name" / "  [symbol stub]"。イメージ外は空。
std::string formatDescribe(const SymbolRef& r);

// v1 Session::symbolName と同じ文字列: "func" / "func+0xNN" / "name [stub]"。
// シンボルが無ければイメージ名。イメージ外は空。
std::string formatSymbolName(const SymbolRef& r);

} // namespace ta2
