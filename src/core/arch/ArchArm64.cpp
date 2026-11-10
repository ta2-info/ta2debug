// arm64 (Apple シリコン) 向け Arch 実装。-arch arm64 でのみコンパイルされる。
// このマシン（x86_64）では実行できないため、現状はクロスコンパイル確認のみ。
// 実機での動作検証（ブレイク・ステップ・HW ブレイク）は arm64 Mac で行う。
#include "core/Arch.h"

#if defined(__arm64__) || defined(__aarch64__)

#include <cstring>
#include <mach/arm/thread_status.h>
#include <mach-o/compact_unwind_encoding.h>
#include <capstone/capstone.h>

namespace ta2 {
namespace {

// 名前 → arm_thread_state64_t の 64bit レジスタへのポインタ（x0..x30, sp, pc）
uint64_t* slot(arm_thread_state64_t* s, const std::string& n) {
  if (n.size() >= 2 && n[0] == 'x') {
    char* end = nullptr;
    long i = strtol(n.c_str() + 1, &end, 10);
    if (end && *end == '\0' && i >= 0 && i <= 28) return &s->__x[i];
    if (n == "x29") return &s->__fp;
    if (n == "x30") return &s->__lr;
  }
  if (n == "fp") return &s->__fp;
  if (n == "lr") return &s->__lr;
  if (n == "sp") return &s->__sp;
  if (n == "pc") return &s->__pc;
  return nullptr;
}

// w0..w30 → 対応する x レジスタ名（32bit 別名）
std::string base32(const std::string& n) {
  if (!n.empty() && n[0] == 'w') return "x" + n.substr(1);
  return "";
}

class ArchArm64 : public Arch {
public:
  const char* name() const override { return "arm64"; }
  cpu_type_t cpuType() const override { return CPU_TYPE_ARM64; }

  thread_state_flavor_t threadFlavor() const override { return ARM_THREAD_STATE64; }
  mach_msg_type_number_t threadStateCount() const override { return ARM_THREAD_STATE64_COUNT; }
  thread_state_flavor_t debugFlavor() const override { return ARM_DEBUG_STATE64; }
  mach_msg_type_number_t debugStateCount() const override { return ARM_DEBUG_STATE64_COUNT; }

  thread_state_flavor_t floatFlavor() const override { return ARM_NEON_STATE64; }
  mach_msg_type_number_t floatStateCount() const override { return ARM_NEON_STATE64_COUNT; }
  std::vector<VecRegValue> vectorRegisters(const void* fs) const override {
    auto* s = reinterpret_cast<const arm_neon_state64_t*>(fs);
    std::vector<VecRegValue> out;
    for (int i = 0; i < 32; i++) {
      std::vector<uint8_t> b(16);
      memcpy(b.data(), &s->__v[i], 16); // V レジスタは 128bit
      out.push_back({"v" + std::to_string(i), std::move(b)});
    }
    return out;
  }

  std::vector<RegValue> debugRegisters(const void* ds) const override {
    auto* d = reinterpret_cast<const arm_debug_state64_t*>(ds);
    std::vector<RegValue> out;
    for (int i = 0; i < 6; i++) { // 実行ブレイク（値/制御）
      out.push_back({"bvr" + std::to_string(i), d->__bvr[i], 64, false});
      out.push_back({"bcr" + std::to_string(i), d->__bcr[i], 64, true});
    }
    for (int i = 0; i < 4; i++) { // ウォッチ（値/制御）
      out.push_back({"wvr" + std::to_string(i), d->__wvr[i], 64, false});
      out.push_back({"wcr" + std::to_string(i), d->__wcr[i], 64, true});
    }
    out.push_back({"mdscr_el1", d->__mdscr_el1, 64, true}); // 単一ステップ(SS)等
    return out;
  }

  // 注: arm64e は PAC により __pc/__lr に署名が載る。将来 ptrauth_strip が要る。
  uint64_t pc(const void* ts) const override { return cst(ts)->__pc; }
  void setPc(void* ts, uint64_t v) const override { mut(ts)->__pc = v; }
  uint64_t sp(const void* ts) const override { return cst(ts)->__sp; }
  uint64_t fp(const void* ts) const override { return cst(ts)->__fp; } // x29
  bool retAddrAtEntryOnStack() const override { return false; }        // 戻り先は LR(x30)
  uint64_t linkRegister(const void* ts) const override { return cst(ts)->__lr; } // x30

  // compact unwind 1 エンコーディングから呼び出し元フレームを計算。
  bool stepUnwind(uint32_t enc, uint64_t /*funcStart*/, const UnwindRegs& in,
                  UnwindRegs& out, const MemReader& read) const override {
    switch (enc & UNWIND_ARM64_MODE_MASK) {
      case UNWIND_ARM64_MODE_FRAME: {
        // フレームレコード {fp, lr} が [x29] にある。CFA = x29+16。
        if (!in.fp) return false;
        uint64_t caller = 0, ra = 0;
        if (!read(in.fp, &caller, 8) || !read(in.fp + 8, &ra, 8) || !ra) return false;
        out.pc = ra; out.fp = caller; out.sp = in.fp + 16;
        return true;
      }
      case UNWIND_ARM64_MODE_FRAMELESS: {
        // leaf（呼び出しをしない）関数。戻り先は LR に残っている（最内フレームでのみ有効）。
        uint64_t stackSize = (uint64_t)((enc & UNWIND_ARM64_FRAMELESS_STACK_SIZE_MASK) >> 12) * 16;
        if (!in.lr) return false;
        out.pc = in.lr; out.sp = in.sp + stackSize; out.fp = in.fp;
        return true;
      }
      default: // UNWIND_ARM64_MODE_DWARF / 無し
        return false;
    }
  }

  bool unwindIsDwarf(uint32_t enc) const override {
    return (enc & UNWIND_ARM64_MODE_MASK) == UNWIND_ARM64_MODE_DWARF;
  }
  uint32_t unwindDwarfOffset(uint32_t enc) const override {
    return enc & UNWIND_ARM64_DWARF_SECTION_OFFSET;
  }
  int dwarfRegSP() const override { return 31; } // sp
  int dwarfRegFP() const override { return 29; } // x29
  int dwarfRegRA() const override { return 30; } // x30(lr)

  uint64_t syscallNumberReg(const void* ts) const override { return cst(ts)->__x[16]; } // x16
  uint64_t syscallArgReg(const void* ts, int i) const override {
    return (i >= 0 && i <= 5) ? cst(ts)->__x[i] : 0; // x0..x5
  }

  std::vector<RegValue> registers(const void* ts) const override {
    auto* s = const_cast<arm_thread_state64_t*>(cst(ts));
    std::vector<RegValue> out;
    char buf[8];
    for (int i = 0; i <= 28; i++) { snprintf(buf, sizeof(buf), "x%d", i); out.push_back({buf, s->__x[i], 64, false}); }
    out.push_back({"fp", s->__fp, 64, false});   // x29
    out.push_back({"lr", s->__lr, 64, false});   // x30
    out.push_back({"sp", s->__sp, 64, false});
    out.push_back({"pc", s->__pc, 64, false});
    out.push_back({"cpsr", s->__cpsr, 32, true});
    return out;
  }

  bool readRegisterByName(const void* ts, const std::string& name, uint64_t& out) const override {
    auto* s = const_cast<arm_thread_state64_t*>(cst(ts));
    if (name == "cpsr") { out = s->__cpsr; return true; }
    if (uint64_t* p = slot(s, name)) { out = *p; return true; }
    std::string b = base32(name);
    if (!b.empty()) { if (uint64_t* p = slot(s, b)) { out = *p & 0xffffffffULL; return true; } }
    return false;
  }

  bool writeRegisterByName(void* ts, const std::string& name, uint64_t value) const override {
    if (uint64_t* p = slot(mut(ts), name)) { *p = value; return true; }
    std::string b = base32(name); // w0..w30: 下位 32bit を書き、上位は保持
    if (!b.empty()) { if (uint64_t* p = slot(mut(ts), b)) { *p = (*p & ~0xffffffffULL) | (value & 0xffffffffULL); return true; } }
    return false;
    // 注: arm64 GPR は 16/8bit の名前付きサブレジスタを持たない（w=32bit のみ）。
  }

  // BRK #0。arm64 命令は 4 byte 固定、リトルエンディアンで D4 20 00 00 → {00,00,20,D4}
  std::vector<uint8_t> softBreakBytes() const override { return {0x00, 0x00, 0x20, 0xD4}; }
  int softBreakPcBackup() const override { return 0; } // BRK は ELR=命令自身

  // 単一ステップ: MDSCR_EL1 の SS ビット(bit0)。デバッグ状態側。
  bool singleStepUsesDebugState() const override { return true; }
  void setSingleStep(void* ds, bool on) const override {
    auto* d = mutd(ds);
    if (on) d->__mdscr_el1 |= 0x1ULL;
    else    d->__mdscr_el1 &= ~0x1ULL;
  }

  int hwBreakCount() const override { return 6; } // 実機の実装本数は ID_AA64DFR0 で要確認

  void setHwExecBreak(void* ds, int i, uint64_t address) const override {
    if (i < 0 || i >= 16) return;
    auto* d = mutd(ds);
    d->__bvr[i] = address;
    // DBGBCR: E(bit0)=1, PMC(bits1-2)=0b10(EL0), BAS(bits5-8)=0b1111, BT=0(命令アドレス一致)
    d->__bcr[i] = (0xFULL << 5) | (0x2ULL << 1) | 0x1ULL; // = 0x1E5
  }

  void clearHwExecBreak(void* ds, int i) const override {
    if (i < 0 || i >= 16) return;
    auto* d = mutd(ds);
    d->__bvr[i] = 0;
    d->__bcr[i] = 0;
  }

  // どの番号がヒットしたかは debug state だけでは分からない。PC 一致で判定する前提。
  int hwBreakHit(const void*) const override { return -1; }
  // arm64 はウォッチの発火状態を debug state から消す必要はない（no-op）。

  // arm64 は実行ブレイク(DBGBVR/BCR)とウォッチ(DBGWVR/WCR)が別バンク。
  int hwWatchCount() const override { return 4; }       // 実装本数は ID_AA64DFR0.WRPs で要確認
  bool hwExecWatchSharePool() const override { return false; }
  bool hwWatchStopsBeforeAccess() const override { return true; } // アクセス命令で止まり再実行する

  void setHwWatch(void* ds, int i, uint64_t address, int size, bool onRead, bool onWrite) const override {
    if (i < 0 || i >= 16) return;
    auto* d = mutd(ds);
    uint64_t aligned = address & ~0x7ULL;          // WVR はダブルワード境界
    uint64_t off = address & 0x7ULL;
    uint64_t bas = ((size >= 8) ? 0xFFULL : (((1ULL << size) - 1) << off)) & 0xFFULL; // 監視バイト選択
    uint64_t lsc = (onRead ? 0x1ULL : 0) | (onWrite ? 0x2ULL : 0); if (!lsc) lsc = 0x2ULL;
    d->__wvr[i] = aligned;
    // DBGWCR: E(bit0)=1, PAC(bits1-2)=0b10(EL0), LSC(bits3-4), BAS(bits5-12)
    d->__wcr[i] = (bas << 5) | (lsc << 3) | (0x2ULL << 1) | 0x1ULL;
  }

  void clearHwWatch(void* ds, int i) const override {
    if (i < 0 || i >= 16) return;
    auto* d = mutd(ds);
    d->__wvr[i] = 0;
    d->__wcr[i] = 0;
  }

  int csArch() const override { return CS_ARCH_ARM64; }
  int csMode() const override { return CS_MODE_LITTLE_ENDIAN; }

  InsnKind classify(const std::string& m, bool isCall, bool isRet) const override {
    if (isCall) return InsnKind::Call;   // bl, blr
    if (isRet)  return InsnKind::Return; // ret
    if (m == "svc") return InsnKind::Syscall;
    return InsnKind::Other;
  }

private:
  static const arm_thread_state64_t* cst(const void* p) { return reinterpret_cast<const arm_thread_state64_t*>(p); }
  static arm_thread_state64_t* mut(void* p) { return reinterpret_cast<arm_thread_state64_t*>(p); }
  static arm_debug_state64_t* mutd(void* p) { return reinterpret_cast<arm_debug_state64_t*>(p); }
};

} // namespace

std::unique_ptr<Arch> Arch::createForHost() { return std::make_unique<ArchArm64>(); }

std::unique_ptr<Arch> Arch::create(cpu_type_t cpu) {
  if (cpu == CPU_TYPE_ARM64) return std::make_unique<ArchArm64>();
  return nullptr;
}

} // namespace ta2

#endif // __arm64__
