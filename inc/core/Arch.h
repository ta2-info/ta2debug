#pragma once

// アーキ依存の一切を隠す抽象。設計書 §4 を参照。
//   ArchX86_64 : x86_64 実装（-arch x86_64 でのみコンパイル）
//   ArchArm64  : arm64 実装 （-arch arm64 でのみコンパイル）
// あるアーキの Arch 実装は、そのアーキの -arch でのみコンパイルできる
// （mach の arm_thread_state64_t 等が __arm64__ でガードされているため）。
// どの実装がバイナリに入るかはホスト（ビルド）アーキで決まる。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <mach/mach.h>

namespace ta2 {

// 表示・式評価用の 1 レジスタ
struct RegValue {
  std::string name;   // 小文字。x86: "rax"、arm64: "x0" など
  uint64_t value = 0;
  uint8_t bits = 64;  // 64 / 32
  bool isFlags = false; // rflags / cpsr か（表示のグルーピング用）
};

// ベクタ/浮動小数レジスタ（x86: xmm0..15、arm64: v0..31）。128bit なので生バイトで持つ。
struct VecRegValue {
  std::string name;              // "xmm0" / "v0" など
  std::vector<uint8_t> bytes;    // 16 バイト（下位→上位）
};

// CFI アンワインド用の最小レジスタ集合（バックトレースに必要なものだけ）。
struct UnwindRegs {
  uint64_t pc = 0; // rip / pc
  uint64_t sp = 0; // rsp / sp
  uint64_t fp = 0; // rbp / x29
  uint64_t lr = 0; // x30（arm64 のみ。x86 は未使用）
};
// 対象メモリ読取（true=成功）。Arch はこれ経由でのみ対象に触れる（Target 非依存を保つ）。
using MemReader = std::function<bool(uint64_t addr, void* buf, size_t len)>;

// next/auto で使う命令分類（アーキ非依存の呼び名）
enum class InsnKind {
  Other,
  Call,     // x86: call / arm64: bl, blr
  Return,   // ret
  Syscall,  // x86: syscall, sysenter, int / arm64: svc
};

// アーキ依存を隠すインタフェース。モデルはこの型だけを見る。
class Arch {
public:
  virtual ~Arch() = default;

  // --- 識別 ---
  virtual const char* name() const = 0;       // "x86_64" / "arm64"
  virtual cpu_type_t cpuType() const = 0;

  // --- スレッド状態（Target が thread_get_state/set_state に渡す） ---
  // threadState バッファは threadStateCount()*sizeof(natural_t) バイト。
  // 実体は x86_thread_state64_t / arm_thread_state64_t（実装が cast する）。
  virtual thread_state_flavor_t threadFlavor() const = 0;
  virtual mach_msg_type_number_t threadStateCount() const = 0;
  virtual thread_state_flavor_t debugFlavor() const = 0;
  virtual mach_msg_type_number_t debugStateCount() const = 0;
  // ベクタ/浮動小数レジスタの状態（別フレーバ）。未対応なら 0 を返す（既定）。
  virtual thread_state_flavor_t floatFlavor() const { return 0; }
  virtual mach_msg_type_number_t floatStateCount() const { return 0; }
  // 上のフレーバで取得した状態バッファから xmm0..15 / v0..31 を取り出す（既定は空）。
  virtual std::vector<VecRegValue> vectorRegisters(const void* /*floatState*/) const { return {}; }
  // デバッグレジスタの生の値を取り出す（x86: dr0-3/dr6/dr7、arm64: bvr/bcr/wvr/wcr）。表示用。
  virtual std::vector<RegValue> debugRegisters(const void* /*debugState*/) const { return {}; }

  // --- 主要レジスタ ---
  virtual uint64_t pc(const void* threadState) const = 0;   // rip / pc
  virtual void setPc(void* threadState, uint64_t v) const = 0;
  virtual uint64_t sp(const void* threadState) const = 0;   // rsp / sp
  virtual uint64_t fp(const void* threadState) const = 0;   // rbp / x29（bt 用）

  // 関数プロローグ実行前（call/bl 直後。まだ rbp/x29 を積んでいない）に止まったとき、
  // 呼び出し元への戻りアドレスの在り処。異常ジャンプ（IP=0 等）のバックトレース復元に使う。
  //   x86: call が戻り先を [sp] に積む → true（呼び出し側が [sp] を読む）。
  //   arm64: bl が戻り先を LR(x30) に入れる → false（linkRegister で返す）。
  virtual bool retAddrAtEntryOnStack() const = 0;
  virtual uint64_t linkRegister(const void* /*threadState*/) const { return 0; } // arm64 の x30

  // CFI（compact unwind）の 1 エンコーディングから呼び出し元フレームを計算する。
  //   encoding : __unwind_info の 32bit コンパクトエンコーディング
  //   funcStart: その関数の実行時先頭アドレス（STACK_IND が関数本体の即値を読むのに使う）
  //   in       : 現フレームのレジスタ、out: 呼び出し元の pc/sp/fp（lr は不定）
  //   read     : 対象メモリ読取
  // 戻り値 false = DWARF モードや未対応・読取失敗（呼び出し側がヒューリスティックへフォールバック）。
  virtual bool stepUnwind(uint32_t encoding, uint64_t funcStart,
                          const UnwindRegs& in, UnwindRegs& out,
                          const MemReader& read) const = 0;

  // compact unwind が DWARF モード（__eh_frame 参照）か、そのときの FDE オフセット。
  virtual bool unwindIsDwarf(uint32_t encoding) const = 0;
  virtual uint32_t unwindDwarfOffset(uint32_t encoding) const = 0;
  // DWARF レジスタ番号（CFI 解釈で sp/fp/戻りアドレスを判定するのに使う）。
  virtual int dwarfRegSP() const = 0;
  virtual int dwarfRegFP() const = 0;
  virtual int dwarfRegRA() const = 0;

  // システムコール呼び出し直前の解析用。番号レジスタ（x86:rax / arm64:x16）と
  // 引数レジスタ（index 0..5。x86:rdi,rsi,rdx,r10,r8,r9 / arm64:x0..x5）の値。
  virtual uint64_t syscallNumberReg(const void* threadState) const = 0;
  virtual uint64_t syscallArgReg(const void* threadState, int index) const = 0;

  // --- レジスタ一覧（表示）と名前引き（式・setreg） ---
  virtual std::vector<RegValue> registers(const void* threadState) const = 0;
  virtual bool readRegisterByName(const void* threadState, const std::string& name, uint64_t& out) const = 0;
  virtual bool writeRegisterByName(void* threadState, const std::string& name, uint64_t value) const = 0;

  // --- ソフトブレイク命令のバイト列（x86:CC / arm64:BRK #0 の 4byte） ---
  virtual std::vector<uint8_t> softBreakBytes() const = 0;
  // ソフトブレイク発生後、PC がブレイク命令より何バイト先にあるか。
  //   x86: INT3 は #BP で rip=命令の次 → 1 戻す。arm64: BRK は ELR=命令自身 → 0。
  virtual int softBreakPcBackup() const = 0;

  // --- 単一ステップ ---
  // x86 はスレッド状態(RFLAGS の TF)、arm64 はデバッグ状態(MDSCR の SS) に入る。
  // setSingleStep に渡すバッファは singleStepUsesDebugState() が false なら thread、true なら debug。
  virtual bool singleStepUsesDebugState() const = 0;
  virtual void setSingleStep(void* state, bool on) const = 0;

  // --- ハードウェア実行ブレイク（debugState を書き換える） ---
  virtual int hwBreakCount() const = 0;                 // 使える本数（x86:4）
  virtual void setHwExecBreak(void* debugState, int index, uint64_t address) const = 0;
  virtual void clearHwExecBreak(void* debugState, int index) const = 0;
  // 直近で発火したデバッグレジスタ番号（実行/データ兼。分からなければ -1）。
  // x86 は DR6 の B0..B3 で判る。arm64 は debugState だけでは判らず -1（PC 一致などで判定）。
  virtual int hwBreakHit(const void* debugState) const = 0;
  // 発火状態をクリア（次の判定が古いビットに惑わされないように）。x86:DR6=0 / arm64:何もしない。
  virtual void clearHwHitStatus(void* /*debugState*/) const {}

  // --- ハードウェアウォッチポイント（データブレイク。debugState を書き換える） ---
  virtual int hwWatchCount() const = 0;                 // 使える本数（x86:4 / arm64:実装依存）
  // x86 は実行ブレイクとウォッチが同じ DR0..DR3 を共用する（true）。arm64 は別バンク（false）。
  virtual bool hwExecWatchSharePool() const = 0;
  // ウォッチがアクセス命令の「手前」で止まり再開で再発火するか。
  //   x86: アクセス後のトラップなので false（そのまま継続でよい）。
  //   arm64: アクセス命令で止まり再実行するため true（1 命令だけ外して跨ぐ必要がある）。
  virtual bool hwWatchStopsBeforeAccess() const = 0;
  // index 番のウォッチを設定。size は 1/2/4/8 バイト、address は size 境界に整列していること。
  virtual void setHwWatch(void* debugState, int index, uint64_t address, int size,
                          bool onRead, bool onWrite) const = 0;
  virtual void clearHwWatch(void* debugState, int index) const = 0;

  // --- 逆アセンブラ（Disassembler が capstone に渡す。Arch は capstone 型を持たない） ---
  virtual int csArch() const = 0;   // CS_ARCH_X86 / CS_ARCH_ARM64 の整数値
  virtual int csMode() const = 0;   // CS_MODE_64 / CS_MODE_LITTLE_ENDIAN の整数値

  // --- 命令分類（Disassembler が capstone の汎用グループで call/ret を判定して渡す） ---
  virtual InsnKind classify(const std::string& mnemonic, bool isCallGroup, bool isRetGroup) const = 0;

  // --- ファクトリ ---
  // ビルドされたホストアーキの実装を返す（必ず 1 つ返る）。
  static std::unique_ptr<Arch> createForHost();
  // 対象の cpu_type に合う実装を返す。このビルドに無ければ nullptr。
  static std::unique_ptr<Arch> create(cpu_type_t cpuType);
};

} // namespace ta2
