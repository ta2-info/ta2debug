#include "core/Disassembler.h"

#include <capstone/capstone.h>

namespace ta2 {

static bool s_intelSyntax = false; // x86 表示記法（false=AT&T 既定）。プロセス全体で共有。
void Disassembler::setIntelSyntax(bool on) { s_intelSyntax = on; }
bool Disassembler::intelSyntax() { return s_intelSyntax; }

Disassembler::Disassembler(Arch& arch) : arch_(arch) {
  csh h = 0;
  if (cs_open((cs_arch)arch_.csArch(), (cs_mode)arch_.csMode(), &h) != CS_ERR_OK) {
    handle_ = 0;
    return;
  }
  cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
  // x86 のみ表示記法を適用（Intel / AT&T）。arm64 は CS_OPT_SYNTAX 非対応なので触らない。
  if (arch_.csArch() == CS_ARCH_X86) {
    cs_option(h, CS_OPT_SYNTAX, s_intelSyntax ? CS_OPT_SYNTAX_INTEL : CS_OPT_SYNTAX_ATT);
  }
  handle_ = (size_t)h;
}

Disassembler::~Disassembler() {
  if (handle_) { csh h = (csh)handle_; cs_close(&h); }
}

std::vector<Insn> Disassembler::disasm(const uint8_t* code, size_t size, uint64_t addr, size_t maxInsns) {
  std::vector<Insn> out;
  if (!handle_) return out;
  csh h = (csh)handle_;
  cs_insn* insn = cs_malloc(h);
  const uint8_t* p = code;
  size_t remain = size;
  uint64_t pc = addr;
  while (out.size() < maxInsns && cs_disasm_iter(h, &p, &remain, &pc, insn)) {
    Insn ins;
    ins.address = insn->address;
    ins.size = insn->size;
    ins.mnemonic = insn->mnemonic;
    ins.opStr = insn->op_str;

    bool isCall = cs_insn_group(h, insn, CS_GRP_CALL);
    bool isRet = cs_insn_group(h, insn, CS_GRP_RET);
    bool isJump = cs_insn_group(h, insn, CS_GRP_JUMP);
    ins.kind = arch_.classify(insn->mnemonic, isCall, isRet);
    ins.isBranch = (isCall || isJump) && !isRet; // call / jmp（ret は除く）

    // 直値 call/branch の飛び先（アーキ別に operand を見る）
    if (ins.kind == InsnKind::Call && insn->detail) {
      if (arch_.csArch() == CS_ARCH_X86) {
        auto& x = insn->detail->x86;
        if (x.op_count > 0 && x.operands[0].type == X86_OP_IMM) {
          ins.hasCallTarget = true; ins.callTarget = (uint64_t)x.operands[0].imm;
        }
      } else if (arch_.csArch() == CS_ARCH_ARM64) {
        auto& a = insn->detail->arm64;
        if (a.op_count > 0 && a.operands[0].type == ARM64_OP_IMM) {
          ins.hasCallTarget = true; ins.callTarget = (uint64_t)a.operands[0].imm;
        }
      }
    }

    // 間接 call/jmp のオペランド情報（レジスタ状況から飛び先を解くのは UI 側）。
    // x86: *%reg / *disp(base,index,scale) / *disp(%rip)。arm64: blr/br Xn（レジスタのみ）。
    if (ins.isBranch && insn->detail) {
      if (arch_.csArch() == CS_ARCH_X86) {
        auto& x = insn->detail->x86;
        if (x.op_count > 0) {
          const auto& op = x.operands[0];
          if (op.type == X86_OP_REG) {
            ins.indirect = true; ins.branchOp = BranchOp::Reg;
            ins.baseReg = cs_reg_name(h, op.reg);
          } else if (op.type == X86_OP_MEM) {
            ins.indirect = true; ins.branchOp = BranchOp::Mem;
            if (op.mem.base  != X86_REG_INVALID) ins.baseReg  = cs_reg_name(h, op.mem.base);
            if (op.mem.index != X86_REG_INVALID) ins.indexReg = cs_reg_name(h, op.mem.index);
            ins.branchScale = op.mem.scale ? op.mem.scale : 1;
            ins.branchDisp  = op.mem.disp;
          }
        }
      } else if (arch_.csArch() == CS_ARCH_ARM64) {
        auto& a = insn->detail->arm64;
        if (a.op_count > 0 && a.operands[0].type == ARM64_OP_REG) {
          ins.indirect = true; ins.branchOp = BranchOp::Reg;
          ins.baseReg = cs_reg_name(h, a.operands[0].reg);
        }
      }
    }
    out.push_back(std::move(ins));
  }
  cs_free(insn, 1);
  return out;
}

} // namespace ta2
