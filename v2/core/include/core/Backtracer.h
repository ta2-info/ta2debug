#pragma once

// L1 Core（ta2core）: バックトレースの算出。由来は v1 main.cpp の computeBacktrace /
// addrIsCode / addrIsCallReturn / scanStackForReturn（設計書 §4.2）。
// CFI（compact unwind / DWARF）+ 入口・IP=0・スタブのヒューリスティック + fp 連鎖 +
// スタック走査、という手順と判定は v1 のまま。違いは結果の形だけ:
//   v1: 1 フレーム 1 行の文字列（describe 済み + ANSI 淡色の注記）
//   v2: Frame の配列（pc/sp/fp と「どう復元したか」）。色・文字列整形・シンボル表記は含まない。
//       表示側は resolveAddress(frame.pc) と frameNoteMsg(frame.origin) で v1 と同じ行を作れる。
#include <cstdint>
#include <vector>

#include "base/Msg.h"
#include "core/Breakpoints.h"
#include "core/Disassembler.h"
#include "core/ImageList.h"
#include "core/Symbols.h"
#include "platform/Target.h"

namespace ta2 {

// そのフレームをどうやって得たか（v1 の行末注記に対応）。
enum class FrameOrigin {
  Current,    // 最内フレーム（スレッド状態の PC そのもの）。注記なし
  Cfi,        // CFI（compact unwind / DWARF）で正確に 1 段。注記なし
  EntryStack, // プロローグ未実行: 戻り先を [sp] から復元（x86）
  EntryLr,    // プロローグ未実行: 戻り先を LR から復元（arm64）
  FpChain,    // フレームポインタ（rbp / x29）連鎖
  StackScan,  // スタック走査で復元（推定）
};

struct Frame {
  uint64_t pc = 0;
  uint64_t sp = 0;
  uint64_t fp = 0;
  FrameOrigin origin = FrameOrigin::Current;
};

// 注記の Msg（bt.note.*）。注記の無い origin（Current / Cfi）は空の Msg。
Msg frameNoteMsg(FrameOrigin origin);

class Backtracer {
public:
  Backtracer(Target& target, ImageList& images, Symbols& symbols, const Breakpoints& bps)
    : target_(target), images_(images), symbols_(symbols), bps_(bps) {}

  // threadState（arch().threadStateCount()*sizeof(natural_t) バイト）のスレッドを最大 64 段辿る。
  // fast=true のときは高コストなスタック走査フォールバックを使わない（CFI/fp が切れたら打ち切る）。
  // 停止スレッド以外（スレッド選択で覗く別スレッド）は走査が重くなりがちなので fast で軽くする。
  std::vector<Frame> compute(const void* threadState, bool fast = false);

private:
  // アドレスがいずれかのイメージの実行領域（__TEXT）内か。戻りアドレスの妥当性判定に使う。
  bool addrIsCode(uint64_t x) const;
  // x が「call/bl の直後（＝正当な戻りアドレス）」らしいか。
  bool addrIsCallReturn(Disassembler& dis, uint64_t x);
  // from 以上のスタックを上へ走査し、最初の「戻りアドレスらしい」語を返す（見つけた語のアドレスも）。
  bool scanStackForReturn(Disassembler& dis, uint64_t from, uint64_t& outRet, uint64_t& outSlot);

  Target& target_;
  ImageList& images_;
  Symbols& symbols_;
  const Breakpoints& bps_;
};

} // namespace ta2
