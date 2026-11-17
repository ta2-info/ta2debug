#include "core/Disassembler.h"

#include <cstdio>
#include <cstring>

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
  // arm64 は 4 バイト固定長、x86 は 1 バイト単位。capstone が不正命令で止まったら、この単位で
  // 1 つだけ進めて擬似データ行（.long / .byte）を 1 行出し、再同期する。こうすると「不正バイトで
  // 打ち切り → 以降が空白／後方ナビ停止」を防げる（パディング・リテラルプール・データ混在に強い）。
  const size_t step = (arch_.csArch() == CS_ARCH_ARM64) ? 4 : 1;
  while (out.size() < maxInsns && remain >= step) {
    if (!cs_disasm_iter(h, &p, &remain, &pc, insn)) {
      Insn d;
      d.address = pc;
      d.size = (uint8_t)step;
      d.data = true;
      if (step == 4) {
        uint32_t w = 0; memcpy(&w, p, 4);
        char b[16]; snprintf(b, sizeof(b), "0x%08x", w);
        d.mnemonic = ".long"; d.opStr = b;
      } else {
        char b[8]; snprintf(b, sizeof(b), "0x%02x", (unsigned)p[0]);
        d.mnemonic = ".byte"; d.opStr = b;
      }
      out.push_back(std::move(d));
      p += step; remain -= step; pc += step;
      continue;
    }
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

    // call/jmp（ret を除く分岐）の飛び先オペランドを解析する。
    //  - 直値（IMM）: 飛び先が静的に決まる → hasCallTarget/callTarget（call も jmp/Jcc も対象。表示でシンボルを添える）。
    //  - レジスタ/メモリ（間接）: レジスタ状況から解く必要があるので種別だけ拾い、解決は UI 側（PC 行）。
    //    x86: *%reg / *disp(base,index,scale) / *disp(%rip)。arm64: blr/br Xn（レジスタのみ）。
    if (ins.isBranch && insn->detail) {
      if (arch_.csArch() == CS_ARCH_X86) {
        auto& x = insn->detail->x86;
        if (x.op_count > 0) {
          const auto& op = x.operands[0];
          if (op.type == X86_OP_IMM) {
            ins.hasCallTarget = true; ins.callTarget = (uint64_t)op.imm;
          } else if (op.type == X86_OP_REG) {
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
        // 直値分岐（b/bl/b.cond/cbz/cbnz/tbz/tbnz）は飛び先が IMM オペランド（末尾）。
        // cbz/tbz は operand[0] が条件レジスタなので「operand[0]=REG＝間接」とはできない。
        // IMM が無く operand[0] がレジスタのときだけ br/blr Xn（レジスタ間接）。
        int immIdx = -1;
        for (int k = (int)a.op_count - 1; k >= 0; k--)
          if (a.operands[k].type == ARM64_OP_IMM) { immIdx = k; break; }
        if (immIdx >= 0) {
          ins.hasCallTarget = true; ins.callTarget = (uint64_t)a.operands[immIdx].imm;
        } else if (a.op_count > 0 && a.operands[0].type == ARM64_OP_REG) {
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
