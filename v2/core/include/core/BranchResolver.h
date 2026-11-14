#pragma once

// L1 Core（ta2core）: 間接 call / jmp の飛び先解決。由来は v1 main.cpp の resolveIndirectTarget
// （設計書 §4.2）。v1 は PC 相対を `baseReg == "rip" || baseReg == "pc"` の文字列比較で判定して
// いたが、v2 では Disassembler が Arch::isPcRegister で立てた Insn::pcRelative を見る（§9.2）。
#include <cstdint>

#include "core/Disassembler.h"
#include "platform/Target.h"

namespace ta2 {

// 間接 call/jmp（例 callq *%rax、jmp *(%rax,%rcx,8)、callq *0x10(%rip)、blr x8）の飛び先を、
// threadState のレジスタ（必要なら対象メモリ）から解く。解けたら out に入れて true。
//   Reg: レジスタ値そのもの。
//   Mem: 実効アドレス base+index*scale+disp に入っているポインタ（PC 相対の base は次命令アドレス）。
// 間接分岐でない命令（branchOp == None）・レジスタ不明・メモリ読取失敗は false。
// レジスタの読取は Arch::readRegisterByName に委ねるので x86 / arm64 共通。
bool resolveIndirectTarget(Target& target, const Insn& in, const void* threadState, uint64_t& out);

} // namespace ta2
