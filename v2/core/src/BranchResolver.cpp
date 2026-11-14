#include "core/BranchResolver.h"

namespace ta2 {

bool resolveIndirectTarget(Target& target, const Insn& in, const void* state, uint64_t& out) {
  Arch& a = target.arch();
  if (in.branchOp == BranchOp::Reg) {
    return a.readRegisterByName(state, in.baseReg, out); // 飛び先 = レジスタ値
  }
  if (in.branchOp == BranchOp::Mem) {
    uint64_t ea = (uint64_t)in.branchDisp;
    if (!in.baseReg.empty()) {
      uint64_t bv;
      if (in.pcRelative) bv = in.address + in.size; // RIP 相対は次命令基準
      else if (!a.readRegisterByName(state, in.baseReg, bv)) return false;
      ea += bv;
    }
    if (!in.indexReg.empty()) {
      uint64_t iv;
      if (!a.readRegisterByName(state, in.indexReg, iv)) return false;
      ea += iv * (uint64_t)(in.branchScale ? in.branchScale : 1);
    }
    uint64_t ptr = 0; size_t got = 0; // 実効アドレスに入っているポインタが飛び先
    if (!target.readMemory(ea, &ptr, sizeof(ptr), &got) || got < sizeof(ptr)) return false;
    out = ptr; return true;
  }
  return false;
}

} // namespace ta2
