#pragma once

// capstone のラッパ。アーキに応じたモード（Arch::csArch/csMode）でデコードし、
// 命令の分類（call/ret/syscall）と直値 call の飛び先を Arch 経由で解決する。
// capstone 型はこのヘッダに出さない（csh は .cpp に隠す）。
#include <cstdint>
#include <string>
#include <vector>

#include "core/Arch.h"

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
};

class Disassembler {
public:
  explicit Disassembler(Arch& arch);
  ~Disassembler();

  Disassembler(const Disassembler&) = delete;
  Disassembler& operator=(const Disassembler&) = delete;

  // code[0..size) を addr から最大 maxInsns 命令デコード
  std::vector<Insn> disasm(const uint8_t* code, size_t size, uint64_t addr, size_t maxInsns);

  bool ok() const { return handle_ != 0; }

  // x86 の表示記法を Intel / AT&T(既定) で切り替える（プロセス全体の設定。arm64 は無関係）。
  // Disassembler は使うたびに生成されるので、以降に生成される全インスタンスへ効く。
  static void setIntelSyntax(bool on);
  static bool intelSyntax();

private:
  Arch& arch_;
  size_t handle_ = 0; // csh
};

} // namespace ta2
