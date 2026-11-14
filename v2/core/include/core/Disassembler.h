#pragma once

// L1 Core（ta2core）。v1（inc/core/Disassembler.h）との差（設計書 §9.2）:
//  - Insn::pcRelative を追加（Mem オペランドの base が PC か。Arch::isPcRegister で判定）。
//    上位（BranchResolver）は "rip"/"pc" の文字列比較をしない。
//  - 表示記法（Intel / AT&T）は静的フラグをやめ、生成時の引数にした（設定の持ち主は L2 Debugger）。
//    Arch::supportsSyntaxSwitch() が false のアーキでは無視する。
// capstone のラッパ。アーキに応じたモード（Arch::csArch/csMode）でデコードし、
// 命令の分類（call/ret/syscall）と直値 call の飛び先を Arch 経由で解決する。
// capstone 型はこのヘッダに出さない（csh は .cpp に隠す）。
#include <cstdint>
#include <string>
#include <vector>

#include "platform/Arch.h"

namespace ta2 {

// 間接 call/jmp のオペランド種別（レジスタ経由 / メモリ経由）。
//   Reg: 飛び先はそのレジスタの値そのもの（例 call *%rax）。
//   Mem: 実効アドレス base+index*scale+disp に入っているポインタが飛び先（例 call *(%rax)）。
// capstone 型を漏らさないよう、レジスタは小文字名（readRegisterByName と同じ綴り）で持つ。
enum class BranchOp { None, Reg, Mem };

struct Insn {
  uint64_t address = 0;
  uint8_t size = 0;
  std::string mnemonic;
  std::string opStr;
  InsnKind kind = InsnKind::Other;
  bool hasCallTarget = false; // 直値 call/branch の飛び先が分かるか
  uint64_t callTarget = 0;
  // --- 間接分岐（call/jmp）の飛び先をレジスタ状況から解くためのメタ（UI が PC 行で使う） ---
  bool isBranch = false;            // call か jmp（ret は含めない）
  bool indirect = false;            // オペランドが reg/mem（間接）。直値 imm は false
  BranchOp branchOp = BranchOp::None;
  std::string baseReg;              // Reg: 対象レジスタ名 / Mem: base レジスタ名（小文字。"rip"=RIP 相対）
  std::string indexReg;             // Mem: index レジスタ名（無ければ空）
  int branchScale = 1;              // Mem: index 倍率
  int64_t branchDisp = 0;           // Mem: 変位
  bool pcRelative = false;          // Mem: base が PC（x86 の RIP 相対）。実効アドレスの base は「次命令アドレス」
};

class Disassembler {
public:
  // intelSyntax: x86 の表示記法（false=AT&T 既定）。記法切替の無いアーキ（arm64）では無視。
  explicit Disassembler(Arch& arch, bool intelSyntax = false);
  ~Disassembler();

  Disassembler(const Disassembler&) = delete;
  Disassembler& operator=(const Disassembler&) = delete;

  // code[0..size) を addr から最大 maxInsns 命令デコード
  std::vector<Insn> disasm(const uint8_t* code, size_t size, uint64_t addr, size_t maxInsns);

  bool ok() const { return handle_ != 0; }

  // このインスタンスの表示記法（生成時に決まる。記法切替の無いアーキでは常に false）。
  bool intelSyntax() const { return intel_; }

private:
  Arch& arch_;
  size_t handle_ = 0; // csh
  bool intel_ = false;
};

} // namespace ta2
