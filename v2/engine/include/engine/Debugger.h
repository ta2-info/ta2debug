#pragma once

// L2 Facade/API（ta2engine）: ヘッドレスなデバッガ本体の唯一の公開窓口（設計書 §6.1 / §7）。
//   - コマンドは非同期投入（post）。結果は公開イベント CommandFinished{RequestId, CommandResult}。
//   - 参照は同期クエリ（L1 の構造化結果を公開形で返す）。どのスレッドからでも呼べる。
//   - 出来事は IEventSink へ（StateChanged / Stopped / TargetExited / BreakpointsChanged /
//     ImagesChanged / Progress / CommandFinished / Notice）。
// TargetSession と各サービス（BreakpointService / AutoRunner / JobRunner / StopProcessor）を所有する。
// UI（端末・ANSI・言語カタログ）には依存しない。文はすべて Msg。
//
// スレッドモデル（EngineOptions::threaded）:
//   threaded = true : Debugger がエンジンスレッドを持ち、受信箱（EngineInbox）をそこで処理する。
//   threaded = false: インライン実行。呼び出し側が pump() / pumpWait() を呼んだスレッドで処理する。
// どちらも処理の実体（processItem）は同じ。対象の論理状態は stateMu_ で直列化する:
//   - 受信箱の 1 件の処理と各クエリは stateMu_ を取る（L1 の遅延キャッシュ＝デマングル・DWARF 行・
//     注釈の読み直しがクエリでも書き込みになるので、共有ロックではなく排他にしている）。
//   - L1 の内部スレッドは受信箱へ積むだけで stateMu_ を取らない（SessionSink の約束）。
//   - 公開イベントは stateMu_ を放してから配る（購読側がクエリを呼んでもデッドロックしない）。
//   - 対象を手放す間（例外スレッドの ack 待ち・内部スレッドの join）は teardown_ を立て、
//     その間に来たクエリは待たずに空を返す（§12 のデッドロック / 固着の緩和）。
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

#include "base/Msg.h"
#include "core/TargetSession.h"
#include "engine/AutoRunner.h"
#include "engine/BreakpointService.h"
#include "engine/Command.h"
#include "engine/EngineInbox.h"
#include "engine/Event.h"
#include "engine/JobRunner.h"
#include "engine/StopProcessor.h"
#include "engine/Types.h"

namespace ta2 {
namespace engine {

class Debugger {
public:
  explicit Debugger(EngineOptions opts = EngineOptions());
  // ジョブ中止 → エンジンスレッド停止 → detach（dtrace 停止 → 先読み停止 → 終了監視停止 →
  // 例外スレッドへ Detach を渡して ack 待ち → 例外ポート復元）。
  ~Debugger();

  Debugger(const Debugger&) = delete;
  Debugger& operator=(const Debugger&) = delete;

  const EngineOptions& options() const { return opts_; }

  // ---- イベント購読 ----
  SubscriptionId subscribe(IEventSink* sink);
  void unsubscribe(SubscriptionId id);

  // ---- コマンド（非同期。結果は CommandFinished{RequestId, ...} で届く） ----
  RequestId post(Command cmd);
  // 走行中ジョブ（シンボル探索）を中止する。v1 の ESC。id=0 は「いま走っているジョブ」。
  void cancel(RequestId id = 0);
  // v1 の Ctrl+C。auto 中なら auto 中断、Running なら割り込み停止、Stopped / Idle なら無視。
  void interrupt();

  // ---- インライン実行（threaded=false のときだけ有効。単一スレッドから呼ぶ。onEvent からの再入は不可） ----
  // 受信箱にあるものを全部処理する。処理した件数を返す。
  size_t pump();
  // 最大 timeoutMs（<0 は無期限）待って、来たものを全部処理する。1 件も来なければ false。
  bool pumpWait(int timeoutMs);

  // コマンド実行前の境界で行う最新化（v1 runCommand 冒頭の refreshImages + refreshAnnotations）。
  // テキストコマンドのうちクエリだけで済むもの（:img / :syms など）の前に CommandInterpreter が呼ぶ。
  // ジョブ走行中・未アタッチなら何もしない。
  void refresh();

  // ---- クエリ（同期・どのスレッドからでも可） ----
  RunState state() const { return state_.load(); }
  bool attached() const { return attached_.load(); }
  pid_t pid() const { return pid_.load(); }
  bool canRestart();                     // :run で起動した対象があるか（R キーの可否）
  bool intelSyntax() const { return intel_.load(); }
  ArchInfo arch();
  StopInfo stopInfo();                   // 直近の確定停止（Stopped 以外では既定値）
  std::vector<ThreadInfo> threads();     // tid 昇順
  RegistersSnapshot registers(ThreadId t = 0);
  bool readRegister(const std::string& name, uint64_t& out, ThreadId t = 0);
  VectorRegisters vectorRegisters(ThreadId t = 0);
  DebugRegisters debugRegisters(ThreadId t = 0);
  // addr=0 なら表示スレッドの PC から。ブレイク命令は元命令で見せる。
  DisasmResult disassemble(uint64_t addr, int count, DisasmOptions o = DisasmOptions(), ThreadId t = 0);
  // top の insns 命令前のアドレス（上スクロール用。v1 disasmBack）。同期できなければ top。
  uint64_t disasmBack(uint64_t top, int insns);
  // renderTop から前方デコードして base の直前命令の先頭を返す（v1 prevInsnAddr。無ければ 0）。
  uint64_t insnBefore(uint64_t renderTop, uint64_t base);
  // a の次の命令アドレス（v1 nextInsnAddr。端なら a）。
  uint64_t insnAfter(uint64_t a);
  std::vector<FrameView> backtrace(ThreadId t = 0, bool fast = false);
  MemoryBlock readMemory(uint64_t addr, size_t len);
  bool examine(const std::string& spec, uint64_t addr, ExamineResult& out, Msg& err);
  bool classicDump(uint64_t addr, int bytes, ExamineResult& out, Msg& err);
  SymbolRef describe(uint64_t addr);
  std::string symbolName(uint64_t addr);
  bool sourceLine(uint64_t addr, SourceLoc& out);
  std::vector<BreakpointInfo> breakpoints();
  PendingInfo pendingBreakpoints();
  std::vector<ImageInfo> images();
  SegmentList segments(const std::string& image);       // 空 = 主実行ファイル
  // image 空 = 既定ライブラリ。filter は表示名 + マングル名への部分一致（大小無視）。limit=0 は無制限。
  SymbolPage symbols(const std::string& image, const std::string& filter, size_t limit = 0);
  AnnotationList annotations(const std::string& image); // 空 = 主実行ファイル
  std::string defaultLib();
  SyscallWatchStatus syscallWatch();
  JobStatus job() const { return jobs_.status(); }
  WarmStatus symbolWarming();
  static std::vector<ProcessInfo> processes();          // ピッカー用
  static bool syscallNames(const std::string& filter, SyscallNameList& out, Msg& err);

private:
  using EvalApply = JobRunner::Apply;

  // ---- 受信箱の処理 ----
  void engineLoop();
  void processItem(InboxItem& item);   // stateMu_ を取って handleItem → 放してからイベント配信
  void handleItem(InboxItem& item);
  void handleCommand(RequestId id, Command& c);
  void handleRawStop(const RawStop& raw);
  void handleTargetExited();
  void handleJobDone();
  void handleInterrupt();

  // ---- 共通 ----
  void emit(DebugEvent ev);
  void flushEvents();
  void setState(RunState s);
  void finish(RequestId id, CommandResult r);
  void finishOk(RequestId id, Msg m = Msg());
  void finishErr(RequestId id, Msg m);
  void detachSession();                // session_.detach() + 世代を進める + 付随状態の初期化
  void refreshLocked();
  void evalThen(RequestId id, const AddrSpec& a, EvalApply apply);
  bool evalSync(const AddrSpec& a, uint64_t& out, Msg& err);
  static Msg exprError(const Msg& err) { return Msg::of(MsgId::ErrExpr).sub("error", err); }
  bool stoppedLocked() { return state_.load() == RunState::Stopped && session_.isAttached() && session_.stepper().stopped(); }
  // 表示スレッド t（0=停止スレッド）のスレッド状態。停止中でなければ nullptr。
  const uint8_t* viewState(ThreadId t, std::vector<uint8_t>& buf);
  bool doLaunch(const std::vector<std::string>& args, bool restoreSaved, Msg& msg);
  void withBreakId(RequestId id, const BpRef& ref, std::function<void(int)> fn);
  void resume(RequestId id, StopProcessor::Resume how);

  // ---- コマンドの実体（v1 runCommand / doAttach / doLaunch / doRestart ほか） ----
  void exec(RequestId id, cmd::Attach& c);
  void exec(RequestId id, cmd::Launch& c);
  void exec(RequestId id, cmd::Restart& c);
  void exec(RequestId id, cmd::Detach& c);
  void exec(RequestId id, cmd::Continue& c);
  void exec(RequestId id, cmd::StepInto& c);
  void exec(RequestId id, cmd::StepOver& c);
  void exec(RequestId id, cmd::Auto& c);
  void exec(RequestId id, cmd::AddBreakpoint& c);
  void exec(RequestId id, cmd::RemoveBreakpoint& c);
  void exec(RequestId id, cmd::ToggleEnabled& c);
  void exec(RequestId id, cmd::ToggleBreakpointAt& c);
  void exec(RequestId id, cmd::RebuildBreakpoint& c);
  void exec(RequestId id, cmd::SaveBreakpoints& c);
  void exec(RequestId id, cmd::SetRegister& c);
  void exec(RequestId id, cmd::WriteMemory& c);
  void exec(RequestId id, cmd::WriteValue& c);
  void exec(RequestId id, cmd::FillMemory& c);
  void exec(RequestId id, cmd::DumpToFile& c);
  void exec(RequestId id, cmd::LoadFromFile& c);
  void exec(RequestId id, cmd::SetAnnotation& c);
  void exec(RequestId id, cmd::RemoveAnnotation& c);
  void exec(RequestId id, cmd::AutoAnnotate& c);
  void exec(RequestId id, cmd::SetDefaultLib& c);
  void exec(RequestId id, cmd::SyscallWatch& c);
  void exec(RequestId id, cmd::SyscallWatchOff& c);
  void exec(RequestId id, cmd::SyscallStop& c);
  void exec(RequestId id, cmd::SyscallStopOff& c);
  void exec(RequestId id, cmd::Evaluate& c);
  void exec(RequestId id, cmd::Describe& c);
  void exec(RequestId id, cmd::Examine& c);
  void exec(RequestId id, cmd::SetDisasmSyntax& c);

  EngineOptions opts_;
  // メンバの並び = 生成順。inbox_ は session_（SessionSink& を握る）より先、サービスは session_ の後。
  EngineInbox inbox_;
  TargetSession session_;
  BreakpointService bps_;
  AutoRunner auto_;
  StopProcessor stops_;
  JobRunner jobs_;

  std::mutex stateMu_;                 // 対象の論理状態（session_ 以下）を直列化
  std::atomic<bool> teardown_{false};  // 対象を手放し中（クエリは待たずに空を返す）
  std::atomic<RunState> state_{RunState::Idle};
  std::atomic<bool> attached_{false};
  std::atomic<pid_t> pid_{0};
  std::atomic<bool> intel_{false};     // 逆アセンブル記法（v1 Disassembler::setIntelSyntax の静的フラグ）
  std::atomic<RequestId> nextRequest_{1};
  StopInfo stopInfo_;                  // 直近の確定停止（stateMu_ 下）
  std::vector<std::string> launchArgs_;// 直近に :run で起動した対象の引数（再起動用）
  std::vector<InboxItem> deferred_;    // ジョブ走行中に来た RawStop / TargetExited（ジョブ完了後に処理）

  std::mutex outMu_;                   // outbox_
  std::vector<DebugEvent> outbox_;     // 配信待ちの公開イベント
  std::mutex subMu_;                   // sinks_
  std::vector<std::pair<SubscriptionId, IEventSink*>> sinks_;
  SubscriptionId nextSub_ = 1;

  std::thread engine_;
  std::atomic<bool> quit_{false};
};

} // namespace engine
} // namespace ta2
