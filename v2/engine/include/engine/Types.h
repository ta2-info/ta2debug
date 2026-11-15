#pragma once

// L2 Facade/API（ta2engine）: 公開値型。設計書 design/REFACTORING.md §6.1〜§6.3。
//   - 実行状態・ID・エンジン設定（EngineOptions）
//   - コマンドが使う小さな値（AddrSpec / BpKind / WatchParams / BpRef）
//   - スナップショット型（StopInfo / DisasmLine / FrameView / BreakpointInfo ほか）と CommandResult
// 表示言語に依存する文字列は含めない（文は Msg、識別子＝シンボル名・レジスタ名・syscall 名はそのまま）。
// L1 の構造化結果（SymbolRef / SyscallCall / ExamineResult / SourceLoc / Segment / Annot / RegValue）は
// そのまま公開形として使う。L3 はこのヘッダ経由で受け取り、core/ platform/ を直接 include しない。
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <sys/types.h>

#include "base/Msg.h"
#include "core/Annotations.h"
#include "core/Backtracer.h"
#include "core/Disassembler.h"
#include "core/DwarfLines.h"
#include "core/ImageList.h"
#include "core/MemoryExaminer.h"
#include "core/SymbolRef.h"
#include "core/SyscallDecoder.h"
#include "platform/Arch.h"
#include "platform/ProcessList.h"

namespace ta2 {
namespace engine {

// 公開される実行状態は 3 つ（Launching / AutoStepping / InternalStop は Running に見える。§7.4）。
enum class RunState { Idle, Running, Stopped };

using RequestId = uint64_t;       // post() が返す。結果は CommandFinished{RequestId, ...}
using SubscriptionId = uint64_t;
using ThreadId = uint32_t;        // thread_act_t を UI に見せるための別名。0 = 停止スレッド

// v1 の g_interactive 分岐を設定として表す（§6.1）。
struct EngineOptions {
  bool warmSymbols = true;            // バッチは false（v1 Session::setWarmingEnabled(false)）
  bool dyldNotifier = true;           // バッチは false（v1 は TUI のみ ensureDyldNotifier）
  bool autoRestoreBreakpoints = true; // バッチは false（v1 は g_restorePending が TUI のみ）
  bool asyncEval = true;              // 式評価をジョブスレッドで行うか。バッチは false（同期評価）
  // true: Debugger がエンジンスレッドを持ち、受信箱をそこで処理する（§7.2）。
  // false: インライン実行。呼び出し側が pump() / pumpWait() で受信箱を処理する（§7.2 末尾）。
  bool threaded = true;

  static EngineOptions interactive() { return EngineOptions(); }
  static EngineOptions batch() {
    EngineOptions o;
    o.warmSymbols = false; o.dyldNotifier = false; o.autoRestoreBreakpoints = false;
    o.asyncEval = false; o.threaded = false;
    return o;
  }
};

// ---- コマンドの部品 ----

// アドレス（または値）。数値でも式でも渡せる。式ならエンジン側で評価する（重い探索はジョブ扱い）。
struct AddrSpec {
  std::variant<uint64_t, std::string> v;
  AddrSpec() : v(uint64_t(0)) {}
  AddrSpec(uint64_t n) : v(n) {}
  AddrSpec(std::string expr) : v(std::move(expr)) {}
  AddrSpec(const char* expr) : v(std::string(expr)) {}
  bool isNumber() const { return std::holds_alternative<uint64_t>(v); }
  uint64_t number() const { return std::get<uint64_t>(v); }
  const std::string& expr() const { return std::get<std::string>(v); }
};

enum class BpKind { Soft, HwExec, Watch };
struct WatchParams { int size = 8; bool onRead = false; bool onWrite = true; };
// ブレイクの指定: #id（int）かアドレス（式可）。
struct BpRef {
  std::variant<int, AddrSpec> v;
  BpRef() : v(0) {}
  BpRef(int id) : v(id) {}
  BpRef(AddrSpec a) : v(std::move(a)) {}
};

// ---- スナップショット ----

struct ArchInfo {
  bool valid = false;
  std::string name;                         // "x86_64" / "arm64"
  std::vector<std::string> registerNames;   // 表示順
  int hwBreakCount = 0;
  int hwWatchCount = 0;
  bool supportsSyntaxSwitch = false;        // Intel / AT&T を切り替えられるか
  std::string vectorFamily;                 // "xmm" / "v"
  int vectorCount = 0;
};

enum class StopKind { Entry, Breakpoint, Watchpoint, Step, Interrupt, Exception, SyscallCatch, Auto, Unknown };
enum class AutoStop { None, Call, Syscall, Return, Limit, Interrupted };

// 1 回の（公開される）停止。v1 の最下行メッセージは次の規則で組まれていた（表示側が再現する）:
//   例外あり: "⚠ " + reason + "  — c で対象を通常終了…" ／ それ以外: reason と syscall の有無で
//   "reason  → syscall" / "reason" / "→ syscall: ..." ／ どちらも無ければ auto の停止文
//   （AutoStopped / AutoStoppedLimit / AutoInterrupted）。その後ろへ notes を「  ／ 」で連結。
struct StopInfo {
  StopKind kind = StopKind::Unknown;
  uint64_t pc = 0;
  ThreadId thread = 0;
  SymbolRef where;                          // describe(pc)
  std::optional<SourceLoc> source;          // DWARF 行（あれば。バッチの "src:"）
  int bpId = -1; uint64_t bpHits = 0; bool bpHw = false;        // PC に有効なユーザブレイク
  int watchId = -1; uint64_t watchAddr = 0; uint64_t watchHits = 0;
  int exception = 0; int64_t code0 = 0; uint64_t faultAddr = 0; // EXC_* と code
  std::string syscallCaught;                // :scstop で捕捉した名前
  Msg reason;                               // stopReasonMsg（例外 / syscall キャッチ。通常停止は空）
  std::optional<SyscallCall> syscall;       // PC が syscall / svc のときの呼び出し内容
  AutoStop autoStop = AutoStop::None; long autoSteps = 0;
  std::vector<Msg> notes;                   // 保存ブレイクの復元件数など
};

struct ThreadInfo { ThreadId id = 0; bool isStopThread = false; };

struct RegistersSnapshot {
  bool valid = false;                       // 停止中でなければ false
  ThreadId thread = 0;
  std::vector<RegValue> regs;
};
struct VectorRegisters {
  bool fetched = false;                     // 状態を取得できたか（false: 取得失敗 / 停止中でない）
  std::vector<VecRegValue> regs;            // 空 = このアーキは未対応
};
struct DebugRegisters {
  bool fetched = false;
  std::vector<RegValue> regs;
};

struct BpMark {
  enum Kind { None, Soft, Hw, Watch } kind = None;
  bool enabled = false;
  int id = 0;
};
struct BranchTarget {
  bool indirect = false;    // 間接 call/jmp を PC 行でレジスタ状況から解いたもの
  bool resolved = true;     // indirect で解けなかったら false（v1 の「飛び先を解決できません」）
  uint64_t addr = 0;
  std::string symbol;       // "func" / "func+0xNN" / "name [stub]"（無ければ空）
};
struct DisasmOptions {
  bool withSource = true;   // DWARF 行が変わる位置に SourceLoc を付ける
  int maxRows = 0;          // >0: 関数見出し・ソース行も 1 行と数えて枠の高さで打ち切る（v1 drawDisassembly）
};
struct DisasmLine {
  uint64_t addr = 0;
  uint8_t size = 0;
  std::string mnemonic, opStr;
  InsnKind kind = InsnKind::Other;
  bool isPc = false;
  BpMark bp;
  std::optional<std::string> funcHeader;    // 関数が変わる行ならその表示名
  bool hasFunc = false; uint64_t funcOffset = 0; // 所属関数先頭からのオフセット
  std::optional<SourceLoc> source;
  std::optional<BranchTarget> target;
  std::optional<SyscallCall> syscall;       // 停止スレッドの PC 行が syscall のとき
  std::string memo;                         // ユーザ注釈のメモ
};
struct DisasmResult {
  bool hasPc = false; uint64_t pc = 0;      // 表示スレッドの PC（停止中のみ）
  std::string image; uint64_t imageBase = 0; // 開始アドレスの所属イメージ（baseoff 表示用。無ければ 0）
  std::vector<DisasmLine> lines;
  uint64_t nextAddr = 0;                    // 最後の行の次の命令アドレス
};

struct FrameView {
  int index = 0;
  uint64_t pc = 0, sp = 0, fp = 0;
  SymbolRef sym;
  FrameOrigin origin = FrameOrigin::Current;
  Msg note;                                 // frameNoteMsg(origin)。注記なしは空
};

struct BreakpointInfo {
  int id = 0;
  uint64_t addr = 0;
  BpKind kind = BpKind::Soft;
  bool enabled = true;
  uint64_t hits = 0;
  int slot = -1;
  int watchSize = 0; bool onRead = false, onWrite = false;
  std::string symbol;                       // symbolName(addr)
};
struct PendingInfo { size_t count = 0; };   // dlopen 待ちの保存ブレイク

struct ImageInfo {
  std::string name, path;
  uint64_t loadAddr = 0, slide = 0, textSize = 0;
  std::string uuid;                         // 大文字 32 桁（無ければ空）
  bool isMain = false;
};
struct SegmentList {
  bool found = false;
  std::string image; uint64_t slide = 0;
  std::vector<Segment> segments;
};
struct SymbolEntry { uint64_t addr = 0; std::string name; };
struct SymbolPage {
  bool found = false;                       // イメージが見つかったか
  std::string image;
  std::vector<SymbolEntry> entries;
  size_t total = 0;                         // 絞込に一致した総数（limit で切る前）
  bool truncated = false;
};
struct AnnotationList {
  bool found = false;
  std::string image; uint64_t slide = 0;
  std::string path;                         // 保存先（UUID 無しなら空＝保存不可）
  std::vector<Annot> annots;                // 実行時アドレス = unslid + slide
};
struct SyscallWatchStatus {
  bool active = false, stopMode = false, traceMode = false;
  std::string filter;                       // 表示用（空なら "all"）
  uint64_t total = 0;
  std::vector<std::pair<std::string, uint64_t>> counts; // 回数の多い順
  std::vector<std::string> trace;                       // 新しい順
};
struct SyscallNameList { std::vector<std::string> bsd, mach; };
struct JobStatus {
  bool running = false;
  bool showing = false;                     // 重い全イメージ探索に入った（進捗表示・ESC 中止の対象）
  int done = 0, total = 0;
  RequestId request = 0;
};
struct WarmStatus { bool active = false; int done = 0, total = 0; };
struct MemoryBlock { bool ok = false; uint64_t addr = 0; std::vector<uint8_t> bytes; };

// m キー（cmd::AutoAnnotate）の結果。existing=true なら既存注釈（表示側が編集行を開く）。
struct AnnotationEdit {
  bool existing = false;
  uint64_t addr = 0;
  std::string name, memo;
};

using ResultPayload = std::variant<std::monostate, ExamineResult, AnnotationEdit>;

struct CommandResult {
  bool ok = false;
  Msg message;                              // v1 の g_message に相当（空可）
  std::optional<uint64_t> value;            // 評価結果・設定したアドレスなど
  std::optional<int> breakpointId;
  std::optional<SymbolRef> where;           // アドレスの記述（v1 が文末に "  " + describe で添えたもの）
  ResultPayload payload;
};

} // namespace engine
} // namespace ta2
