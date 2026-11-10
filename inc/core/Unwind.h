#pragma once

// CFI（compact unwind / __TEXT,__unwind_info）によるスタックアンワインド。
// 各イメージの __unwind_info を読み、PC からコンパクトエンコーディングを引き、
// アーキ依存の復元は Arch::stepUnwind に委ねる（アーキ非依存を保つ）。
// DWARF(__eh_frame)モードや情報欠落時は step() が false を返し、呼び出し側が
// フレームポインタ歩き／スタック走査などのヒューリスティックへフォールバックする。
#include <cstdint>
#include <map>
#include <vector>

#include "core/Arch.h"
#include "core/ImageList.h"

namespace ta2 {

class Unwinder {
public:
  Unwinder(ImageList& images, Arch& arch, MemReader read)
    : images_(images), arch_(arch), read_(std::move(read)) {}

  // in のフレームを 1 段呼び出し元へ進めて out に返す。
  // true=CFI で正確に進めた / false=CFI 情報が無い・DWARF・読取失敗（フォールバックせよ）。
  bool step(const UnwindRegs& in, UnwindRegs& out);

  // pc に対するコンパクトエンコーディングと関数先頭（実行時）を引く。
  bool encodingFor(uint64_t pc, uint32_t& encoding, uint64_t& funcStart);

private:
  const std::vector<uint8_t>* blobFor(const Image& im); // __unwind_info を読んでキャッシュ
  const std::vector<uint8_t>* ehFor(const Image& im);   // __eh_frame を読んでキャッシュ
  // DWARF モード: __eh_frame 内オフセット fdeOff の FDE/CIE を解釈して 1 段進める。
  bool stepDwarf(const Image& im, uint32_t fdeOff, const UnwindRegs& in, UnwindRegs& out);

  ImageList& images_;
  Arch& arch_;
  MemReader read_;
  std::map<uint64_t, std::vector<uint8_t>> cache_;   // loadAddr -> __unwind_info
  std::map<uint64_t, std::vector<uint8_t>> ehCache_; // loadAddr -> __eh_frame
};

} // namespace ta2
