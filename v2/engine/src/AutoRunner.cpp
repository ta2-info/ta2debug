#include "engine/AutoRunner.h"

#include "core/Disassembler.h"

namespace ta2 {
namespace engine {

InsnKind AutoRunner::kindAtPc(TargetSession& s, uint64_t pc) {
  uint8_t code[16] = {}; size_t got = 0;
  if (!s.target().readMemory(pc, code, sizeof(code), &got) || !got) return InsnKind::Other;
  s.maskBreakpoints(pc, code, got);
  Disassembler dis(s.arch());
  auto in = dis.disasm(code, got, pc, 1);
  return in.empty() ? InsnKind::Other : in[0].kind;
}

bool AutoRunner::interrupt() {
  if (!mask_) return false;
  mask_ = 0;            // 実行中の step が次に止まる（v1: g_autoMask = 0）
  interrupted_ = true;
  return true;
}

AutoRunner::Verdict AutoRunner::onStop(TargetSession& s) {
  Verdict v;
  if (!mask_) {
    if (interrupted_) { // 中断後の最初の停止
      interrupted_ = false;
      v.wasAuto = true; v.why = AutoStop::Interrupted; v.steps = steps_;
      steps_ = 0;
    }
    return v;
  }
  v.wasAuto = true;
  uint64_t pc = s.stepper().stopPc();
  bool crash = s.stepper().stopException() != 0;
  const Breakpoint* ub = s.breaks().userAt(pc);
  bool bpHit = ub && ub->enabled;
  InsnKind k = kindAtPc(s, pc);
  bool hit = (k == InsnKind::Call && (mask_ & Call)) ||
             (k == InsnKind::Syscall && (mask_ & Syscall)) ||
             (k == InsnKind::Return && (mask_ & Return));
  if (!crash && !bpHit && !hit && ++steps_ < kAutoMax) {
    v.keepGoing = true; // まだ興味なし → さらに 1 命令
    return v;
  }
  v.steps = steps_; mask_ = 0; steps_ = 0;
  if (!crash && !bpHit)
    v.why = k == InsnKind::Call ? AutoStop::Call : k == InsnKind::Syscall ? AutoStop::Syscall
          : k == InsnKind::Return ? AutoStop::Return : AutoStop::Limit;
  return v;
}

} // namespace engine
} // namespace ta2
