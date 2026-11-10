// x86_64 向け Arch 実装。-arch x86_64 でのみコンパイルされる。
#include "core/Arch.h"

#if defined(__x86_64__)

#include <cstring>
#include <mach/i386/thread_status.h>
#include <mach-o/compact_unwind_encoding.h>
#include <capstone/capstone.h>

namespace ta2 {
namespace {

// x86_thread_state64_t の各レジスタを名前で引くテーブル
struct RegRef { const char* name; uint8_t bits; };

// 64bit レジスタ（表示順）。32bit 別名は読み出しのみ別途対応。
const RegRef kRegs[] = {
  {"rax",64},{"rbx",64},{"rcx",64},{"rdx",64},
  {"rdi",64},{"rsi",64},{"rbp",64},{"rsp",64},
  {"r8",64},{"r9",64},{"r10",64},{"r11",64},
  {"r12",64},{"r13",64},{"r14",64},{"r15",64},
  {"rip",64},{"rflags",64},{"cs",64},{"fs",64},{"gs",64},
};

// 名前 → x86_thread_state64_t メンバへのポインタ
uint64_t* slot(x86_thread_state64_t* s, const std::string& n) {
  if (n=="rax") return &s->__rax; if (n=="rbx") return &s->__rbx;
  if (n=="rcx") return &s->__rcx; if (n=="rdx") return &s->__rdx;
  if (n=="rdi") return &s->__rdi; if (n=="rsi") return &s->__rsi;
  if (n=="rbp") return &s->__rbp; if (n=="rsp") return &s->__rsp;
  if (n=="r8")  return &s->__r8;  if (n=="r9")  return &s->__r9;
  if (n=="r10") return &s->__r10; if (n=="r11") return &s->__r11;
  if (n=="r12") return &s->__r12; if (n=="r13") return &s->__r13;
  if (n=="r14") return &s->__r14; if (n=="r15") return &s->__r15;
  if (n=="rip") return &s->__rip; if (n=="rflags") return &s->__rflags;
  if (n=="cs")  return &s->__cs;  if (n=="fs")  return &s->__fs;
  if (n=="gs")  return &s->__gs;
  // アーキ非依存の別名（式やスクリプトで arm64 と同じ綴り $pc/$sp/$fp を使えるように）。
  // 注: ここで "sp" は完全な 64bit スタックポインタ（=rsp）。x86 伝統の 16bit sp より
  //     クロスアーキの一貫性を優先する（16/32bit が要るなら spl/esp/rsp を使う）。
  if (n=="pc")  return &s->__rip; if (n=="sp")  return &s->__rsp; if (n=="fp") return &s->__rbp;
  return nullptr;
}

// サブレジスタ別名 → 対応する 64bit レジスタ名・ビットシフト・幅マスク。
// 例: eax→rax(0,0xffffffff) / ax→rax(0,0xffff) / al→rax(0,0xff) / ah→rax(8,0xff) / r9w→r9(0,0xffff)。
// 見つかれば true。読みは (値>>shift)&mask、書きは該当ビットのみ差し替え（他ビットは保持）。
bool aliasInfo(const std::string& n, std::string& base, int& shift, uint64_t& mask) {
  struct A { const char* name; const char* base; int shift; uint64_t mask; };
  static const A tbl[] = {
    {"eax","rax",0,0xffffffffULL},{"ebx","rbx",0,0xffffffffULL},{"ecx","rcx",0,0xffffffffULL},{"edx","rdx",0,0xffffffffULL},
    {"esi","rsi",0,0xffffffffULL},{"edi","rdi",0,0xffffffffULL},{"ebp","rbp",0,0xffffffffULL},{"esp","rsp",0,0xffffffffULL},
    {"ax","rax",0,0xffffULL},{"bx","rbx",0,0xffffULL},{"cx","rcx",0,0xffffULL},{"dx","rdx",0,0xffffULL},
    {"si","rsi",0,0xffffULL},{"di","rdi",0,0xffffULL},{"bp","rbp",0,0xffffULL},{"sp","rsp",0,0xffffULL},
    {"al","rax",0,0xffULL},{"bl","rbx",0,0xffULL},{"cl","rcx",0,0xffULL},{"dl","rdx",0,0xffULL},
    {"sil","rsi",0,0xffULL},{"dil","rdi",0,0xffULL},{"bpl","rbp",0,0xffULL},{"spl","rsp",0,0xffULL},
    {"ah","rax",8,0xffULL},{"bh","rbx",8,0xffULL},{"ch","rcx",8,0xffULL},{"dh","rdx",8,0xffULL},
  };
  for (const auto& a : tbl) if (n == a.name) { base = a.base; shift = a.shift; mask = a.mask; return true; }
  // r8..r15 のサフィックス d(32)/w(16)/b(8)
  if (n.size() >= 3 && n[0] == 'r') {
    char suf = n.back();
    uint64_t m = (suf=='d') ? 0xffffffffULL : (suf=='w') ? 0xffffULL : (suf=='b') ? 0xffULL : 0;
    if (m) {
      std::string num = n.substr(1, n.size() - 2); // "8".."15"
      bool ok = !num.empty(); for (char c : num) if (c < '0' || c > '9') ok = false;
      if (ok) { int v = (num.size()<=2) ? std::stoi(num) : -1; if (v >= 8 && v <= 15) { base = "r" + num; shift = 0; mask = m; return true; } }
    }
  }
  return false;
}

class ArchX86_64 : public Arch {
public:
  const char* name() const override { return "x86_64"; }
  cpu_type_t cpuType() const override { return CPU_TYPE_X86_64; }

  thread_state_flavor_t threadFlavor() const override { return x86_THREAD_STATE64; }
  mach_msg_type_number_t threadStateCount() const override { return x86_THREAD_STATE64_COUNT; }
  thread_state_flavor_t debugFlavor() const override { return x86_DEBUG_STATE64; }
  mach_msg_type_number_t debugStateCount() const override { return x86_DEBUG_STATE64_COUNT; }

  thread_state_flavor_t floatFlavor() const override { return x86_FLOAT_STATE64; }
  mach_msg_type_number_t floatStateCount() const override { return x86_FLOAT_STATE64_COUNT; }
  std::vector<VecRegValue> vectorRegisters(const void* fs) const override {
    auto* s = reinterpret_cast<const x86_float_state64_t*>(fs);
    std::vector<VecRegValue> out;
    auto push = [&](const char* nm, const char* reg) { out.push_back({nm, std::vector<uint8_t>(reg, reg + 16)}); };
#define XMM(n) push("xmm" #n, s->__fpu_xmm##n.__xmm_reg);
    XMM(0)  XMM(1)  XMM(2)  XMM(3)  XMM(4)  XMM(5)  XMM(6)  XMM(7)
    XMM(8)  XMM(9)  XMM(10) XMM(11) XMM(12) XMM(13) XMM(14) XMM(15)
#undef XMM
    return out;
  }

  std::vector<RegValue> debugRegisters(const void* ds) const override {
    auto* d = reinterpret_cast<const x86_debug_state64_t*>(ds);
    return {
      {"dr0", d->__dr0, 64, false}, {"dr1", d->__dr1, 64, false},
      {"dr2", d->__dr2, 64, false}, {"dr3", d->__dr3, 64, false},
      {"dr6", d->__dr6, 64, true},  {"dr7", d->__dr7, 64, true}, // dr6=状態, dr7=制御
    };
  }

  uint64_t pc(const void* ts) const override { return cst(ts)->__rip; }
  void setPc(void* ts, uint64_t v) const override { mut(ts)->__rip = v; }
  uint64_t sp(const void* ts) const override { return cst(ts)->__rsp; }
  uint64_t fp(const void* ts) const override { return cst(ts)->__rbp; }
  bool retAddrAtEntryOnStack() const override { return true; } // call が [rsp] に戻り先を積む

  // compact unwind 1 エンコーディングから呼び出し元フレームを計算（libunwind と同じ算法）。
  bool stepUnwind(uint32_t enc, uint64_t funcStart, const UnwindRegs& in,
                  UnwindRegs& out, const MemReader& read) const override {
    auto rd64 = [&](uint64_t a, uint64_t& v) { return read(a, &v, 8); };
    switch (enc & UNWIND_X86_64_MODE_MASK) {
      case UNWIND_X86_64_MODE_RBP_FRAME: {
        // 標準のフレームポインタ。CFA = rbp+16, 戻り先 = [rbp+8], 呼び出し元 rbp = [rbp]。
        if (!in.fp) return false;
        uint64_t ra = 0, caller = 0;
        if (!rd64(in.fp + 8, ra) || !rd64(in.fp, caller) || !ra) return false;
        out.pc = ra; out.fp = caller; out.sp = in.fp + 16;
        return true;
      }
      case UNWIND_X86_64_MODE_STACK_IMMD:
      case UNWIND_X86_64_MODE_STACK_IND: {
        // frameless（rbp を使わない）。スタックサイズから CFA を求める。
        uint32_t szEnc = (enc & UNWIND_X86_64_FRAMELESS_STACK_SIZE) >> 16;
        uint32_t adjust = (enc & UNWIND_X86_64_FRAMELESS_STACK_ADJUST) >> 13;
        uint32_t regCount = (enc & UNWIND_X86_64_FRAMELESS_STACK_REG_COUNT) >> 10;
        uint32_t perm = (enc & UNWIND_X86_64_FRAMELESS_STACK_REG_PERMUTATION);
        uint64_t stackSize;
        if ((enc & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_STACK_IMMD) {
          stackSize = (uint64_t)szEnc * 8;
        } else {
          uint32_t subl = 0; // 関数先頭 + szEnc にある subq $imm,%rsp の 32bit 即値
          if (!read(funcStart + szEnc, &subl, 4)) return false;
          stackSize = (uint64_t)subl + (uint64_t)adjust * 8;
        }
        // 戻り先はフレーム最上段 [sp + stackSize - 8]、CFA = sp + stackSize。
        uint64_t cfa = in.sp + stackSize;
        uint64_t ra = 0;
        if (!rd64(cfa - 8, ra) || !ra) return false;
        out.pc = ra; out.sp = cfa; out.fp = in.fp; // 既定: rbp 不変（未保存）

        // 保存レジスタの順列を復元し、rbp が保存されていれば呼び出し元 rbp を取り出す。
        if (regCount) {
          int p[6] = {0}, cur = perm;
          switch (regCount) {           // libunwind と同じ分解
            case 6: p[0]=cur/120; cur-=p[0]*120; p[1]=cur/24; cur-=p[1]*24;
                    p[2]=cur/6; cur-=p[2]*6; p[3]=cur/2; cur-=p[3]*2; p[4]=cur; p[5]=0; break;
            case 5: p[0]=cur/120; cur-=p[0]*120; p[1]=cur/24; cur-=p[1]*24;
                    p[2]=cur/6; cur-=p[2]*6; p[3]=cur/2; cur-=p[3]*2; p[4]=cur; break;
            case 4: p[0]=cur/60; cur-=p[0]*60; p[1]=cur/12; cur-=p[1]*12;
                    p[2]=cur/3; cur-=p[2]*3; p[3]=cur; break;
            case 3: p[0]=cur/20; cur-=p[0]*20; p[1]=cur/4; cur-=p[1]*4; p[2]=cur; break;
            case 2: p[0]=cur/5; cur-=p[0]*5; p[1]=cur; break;
            case 1: p[0]=cur; break;
          }
          bool used[7] = {false};       // 1..6 を使用済み管理して実レジスタ番号へ
          int reg[6] = {0};
          for (uint32_t i = 0; i < regCount; i++) {
            int renum = 0;
            for (int u = 1; u < 7; u++)
              if (!used[u]) { if (renum == p[i]) { reg[i] = u; used[u] = true; break; } renum++; }
          }
          // 保存位置: [cfa - 8 - 8*regCount] から順に。番号 6 = RBP。
          uint64_t at = cfa - 8 - 8ULL * regCount;
          for (uint32_t i = 0; i < regCount; i++, at += 8) {
            uint64_t v = 0;
            if (!rd64(at, v)) break;
            if (reg[i] == 6) out.fp = v; // 6=RBP
          }
        }
        return true;
      }
      default: // UNWIND_X86_64_MODE_DWARF / エンコーディング無し
        return false;
    }
  }

  bool unwindIsDwarf(uint32_t enc) const override {
    return (enc & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_DWARF;
  }
  uint32_t unwindDwarfOffset(uint32_t enc) const override {
    return enc & UNWIND_X86_64_DWARF_SECTION_OFFSET;
  }
  int dwarfRegSP() const override { return 7; }  // rsp
  int dwarfRegFP() const override { return 6; }  // rbp
  int dwarfRegRA() const override { return 16; } // rip（戻りアドレス）

  uint64_t syscallNumberReg(const void* ts) const override { return cst(ts)->__rax; }
  uint64_t syscallArgReg(const void* ts, int i) const override {
    auto* s = cst(ts);
    switch (i) { // System V/BSD syscall: rcx は syscall 命令で壊れるため arg4 は r10
      case 0: return s->__rdi; case 1: return s->__rsi; case 2: return s->__rdx;
      case 3: return s->__r10; case 4: return s->__r8;  case 5: return s->__r9;
      default: return 0;
    }
  }

  std::vector<RegValue> registers(const void* ts) const override {
    auto* s = const_cast<x86_thread_state64_t*>(cst(ts));
    std::vector<RegValue> out;
    for (const auto& r : kRegs) {
      uint64_t* p = slot(s, r.name);
      RegValue v; v.name = r.name; v.value = p ? *p : 0; v.bits = r.bits;
      v.isFlags = (std::string(r.name) == "rflags");
      out.push_back(v);
    }
    return out;
  }

  bool readRegisterByName(const void* ts, const std::string& name, uint64_t& out) const override {
    auto* s = const_cast<x86_thread_state64_t*>(cst(ts));
    if (uint64_t* p = slot(s, name)) { out = *p; return true; }
    std::string b; int shift; uint64_t mask;
    if (aliasInfo(name, b, shift, mask)) { if (uint64_t* p = slot(s, b)) { out = (*p >> shift) & mask; return true; } }
    return false;
  }

  bool writeRegisterByName(void* ts, const std::string& name, uint64_t value) const override {
    auto* s = mut(ts);
    if (uint64_t* p = slot(s, name)) { *p = value; return true; } // 64bit はそのまま
    std::string b; int shift; uint64_t mask;
    if (aliasInfo(name, b, shift, mask)) {
      if (uint64_t* p = slot(s, b)) { *p = (*p & ~(mask << shift)) | ((value & mask) << shift); return true; } // 該当ビットのみ差替
    }
    return false;
  }

  std::vector<uint8_t> softBreakBytes() const override { return {0xCC}; } // INT3
  int softBreakPcBackup() const override { return 1; } // #BP で rip は次命令

  bool singleStepUsesDebugState() const override { return false; } // TF はスレッド状態
  void setSingleStep(void* ts, bool on) const override {
    if (on) mut(ts)->__rflags |= 0x100ULL;        // TF
    else    mut(ts)->__rflags &= ~0x100ULL;
  }

  int hwBreakCount() const override { return 4; }

  void setHwExecBreak(void* ds, int i, uint64_t address) const override {
    if (i < 0 || i > 3) return;
    auto* d = mutd(ds);
    (&d->__dr0)[i] = address;
    // DR7: ローカル有効ビット(2i)。cond=00(実行), len=00 は該当 4bit を 0 に。
    d->__dr7 |= (1ULL << (i * 2));
    d->__dr7 &= ~(0xFULL << (16 + i * 4));
  }

  void clearHwExecBreak(void* ds, int i) const override {
    if (i < 0 || i > 3) return;
    auto* d = mutd(ds);
    (&d->__dr0)[i] = 0;
    d->__dr7 &= ~(0x3ULL << (i * 2));
  }

  int hwBreakHit(const void* ds) const override {
    uint64_t dr6 = cstd(ds)->__dr6;
    for (int i = 0; i < 4; i++) if (dr6 & (1ULL << i)) return i;
    return -1;
  }
  void clearHwHitStatus(void* ds) const override { mutd(ds)->__dr6 = 0; }

  // x86 は DR0..DR3 を実行ブレイクとウォッチで共用する。
  int hwWatchCount() const override { return 4; }
  bool hwExecWatchSharePool() const override { return true; }
  bool hwWatchStopsBeforeAccess() const override { return false; } // データブレイクはアクセス後のトラップ

  void setHwWatch(void* ds, int i, uint64_t address, int size, bool onRead, bool onWrite) const override {
    if (i < 0 || i > 3) return;
    auto* d = mutd(ds);
    (&d->__dr0)[i] = address;
    d->__dr7 |= (1ULL << (i * 2)); // ローカル有効
    // R/W フィールド: 01=書込のみ / 11=読み書き（x86 に「読み込みのみ」は無いので read は 11 で代用）
    uint64_t rw = onRead ? 0x3ULL : 0x1ULL; // 読みを含むなら読み書き、そうでなければ書込のみ
    // LEN フィールド: 00=1 / 01=2 / 11=4 / 10=8 バイト
    uint64_t len = (size == 8) ? 0x2ULL : (size == 4) ? 0x3ULL : (size == 2) ? 0x1ULL : 0x0ULL;
    uint64_t field = (len << 2) | rw;
    d->__dr7 &= ~(0xFULL << (16 + i * 4));
    d->__dr7 |=  (field << (16 + i * 4));
  }

  void clearHwWatch(void* ds, int i) const override { clearHwExecBreak(ds, i); } // 同じ DR を落とす

  int csArch() const override { return CS_ARCH_X86; }
  int csMode() const override { return CS_MODE_64; }

  InsnKind classify(const std::string& m, bool isCall, bool isRet) const override {
    if (isCall) return InsnKind::Call;
    if (isRet)  return InsnKind::Return;
    if (m == "syscall" || m == "sysenter" || m == "int") return InsnKind::Syscall;
    return InsnKind::Other;
  }

private:
  static const x86_thread_state64_t* cst(const void* p) { return reinterpret_cast<const x86_thread_state64_t*>(p); }
  static x86_thread_state64_t* mut(void* p) { return reinterpret_cast<x86_thread_state64_t*>(p); }
  static const x86_debug_state64_t* cstd(const void* p) { return reinterpret_cast<const x86_debug_state64_t*>(p); }
  static x86_debug_state64_t* mutd(void* p) { return reinterpret_cast<x86_debug_state64_t*>(p); }
};

} // namespace

// このビルド（x86_64）のホスト実装
std::unique_ptr<Arch> Arch::createForHost() { return std::make_unique<ArchX86_64>(); }

std::unique_ptr<Arch> Arch::create(cpu_type_t cpu) {
  if (cpu == CPU_TYPE_X86_64) return std::make_unique<ArchX86_64>();
  return nullptr; // この x86_64 ビルドは他アーキの対象をデバッグできない
}

} // namespace ta2

#endif // __x86_64__
