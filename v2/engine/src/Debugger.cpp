#include "engine/Debugger.h"

#include <utility>

namespace ta2 {
namespace engine {

Debugger::Debugger(EngineOptions opts)
  : opts_(opts),
    session_(inbox_),
    bps_(session_),
    stops_(session_, bps_, auto_, opts_),
    jobs_(session_, inbox_) {
  session_.warmer().setEnabled(opts_.warmSymbols); // バッチは先読みしない
  if (opts_.threaded) engine_ = std::thread([this] { engineLoop(); });
}

Debugger::~Debugger() {
  // 1) ジョブへ中止要求（探索を早く打ち切らせる。フラグを立てるだけ）
  jobs_.cancel();
  // 2) エンジンスレッドを止める（処理中の 1 件が終わってから抜ける。ロックは持たずに join）
  if (engine_.joinable()) {
    InboxItem q;
    q.kind = InboxItem::Quit;
    inbox_.push(std::move(q));
    engine_.join();
  }
  // ジョブのワーカを join（エンジンスレッド停止後なので finish() と競合しない。結果は捨てる）
  jobs_.shutdown();
  quit_ = true;
  // 3) 対象を手放す。順序は TargetSession::detach（dtrace 停止 → 先読み停止 → 終了監視停止 →
  //    例外スレッドへ Detach を渡して ack 待ち → 例外ポート復元 → Target::detach）。
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    detachSession();
    state_ = RunState::Idle;
  }
  // 配り残しのイベントは捨てる（購読側はもう居ない前提）。
}

// ---- 購読 ----

SubscriptionId Debugger::subscribe(IEventSink* sink) {
  std::lock_guard<std::mutex> lk(subMu_);
  SubscriptionId id = nextSub_++;
  sinks_.emplace_back(id, sink);
  return id;
}

void Debugger::unsubscribe(SubscriptionId id) {
  std::lock_guard<std::mutex> lk(subMu_);
  for (auto it = sinks_.begin(); it != sinks_.end(); ++it)
    if (it->first == id) { sinks_.erase(it); break; }
}

void Debugger::emit(DebugEvent ev) {
  std::lock_guard<std::mutex> lk(outMu_);
  outbox_.push_back(std::move(ev));
}

void Debugger::flushEvents() {
  std::vector<DebugEvent> evs;
  { std::lock_guard<std::mutex> lk(outMu_); evs.swap(outbox_); }
  if (evs.empty()) return;
  std::vector<std::pair<SubscriptionId, IEventSink*>> sinks;
  { std::lock_guard<std::mutex> lk(subMu_); sinks = sinks_; }
  for (const auto& ev : evs)
    for (const auto& s : sinks)
      if (s.second) s.second->onEvent(ev);
}

// ---- 投入 ----

RequestId Debugger::post(Command cmd) {
  RequestId id = nextRequest_.fetch_add(1);
  InboxItem it;
  it.kind = InboxItem::Command;
  it.request = id;
  it.command = std::move(cmd);
  inbox_.push(std::move(it));
  return id;
}

void Debugger::cancel(RequestId id) { jobs_.cancel(id); }

void Debugger::interrupt() {
  InboxItem it;
  it.kind = InboxItem::Interrupt;
  inbox_.push(std::move(it));
}

// ---- 受信箱の処理 ----

void Debugger::engineLoop() {
  InboxItem it;
  while (!quit_.load()) {
    if (!inbox_.pop(it, -1)) continue;
    processItem(it);
  }
}

size_t Debugger::pump() {
  if (opts_.threaded) return 0;
  size_t n = 0;
  InboxItem it;
  while (!quit_.load() && inbox_.pop(it, 0)) { processItem(it); n++; }
  return n;
}

bool Debugger::pumpWait(int timeoutMs) {
  if (opts_.threaded) return false;
  InboxItem it;
  if (quit_.load() || !inbox_.pop(it, timeoutMs)) return false;
  processItem(it);
  pump();
  return true;
}

void Debugger::processItem(InboxItem& item) {
  {
    std::lock_guard<std::mutex> lk(stateMu_);
    handleItem(item);
  }
  flushEvents(); // ロックを放してから配る
}

void Debugger::handleItem(InboxItem& item) {
  switch (item.kind) {
    case InboxItem::Command:
      handleCommand(item.request, item.command);
      break;
    case InboxItem::RawStop:
    case InboxItem::TargetExited:
      if (item.generation != inbox_.generation()) break; // 手放した対象の通知は捨てる
      if (jobs_.running()) {
        // 探索ワーカはイメージ一覧・シンボル表を読んでいる。停止の後処理（イメージ更新など）は
        // それらを書き換えるので、ジョブの完了（または中止）まで保留する。
        deferred_.push_back(std::move(item));
        break;
      }
      if (item.kind == InboxItem::RawStop) handleRawStop(item.stop);
      else handleTargetExited();
      break;
    case InboxItem::WarmProgress:
      if (item.generation != inbox_.generation()) break;
      emit(ev::Progress{ev::Progress::Warm, item.done, item.total, item.active});
      break;
    case InboxItem::JobProgress:
      if (jobs_.running()) emit(ev::Progress{ev::Progress::Search, item.done, item.total, true});
      break;
    case InboxItem::JobDone:
      handleJobDone();
      break;
    case InboxItem::Interrupt:
      handleInterrupt();
      break;
    case InboxItem::Quit:
      quit_ = true;
      break;
  }
}

void Debugger::handleCommand(RequestId id, Command& c) {
  // 非同期ジョブ（シンボル探索）実行中は次のコマンドを受けない（対象状態を同時に触らない）。
  if (jobs_.running()) { finishErr(id, Msg::of(MsgId::ErrJobBusy)); return; }
  // コマンド実行前のこの境界で、イメージ一覧とユーザ注釈を最新化する（v1 runCommand 冒頭）。
  refreshLocked();
  std::visit([this, id](auto& x) { exec(id, x); }, c);
}

void Debugger::handleRawStop(const RawStop& raw) {
  StopProcessor::Outcome o = stops_.process(raw);
  switch (o.kind) {
    case StopProcessor::Outcome::Dropped:
      break;
    case StopProcessor::Outcome::Internal:
      // dlopen の透過処理。Stopped は出さない（UI からは Running のまま）。
      emit(ev::ImagesChanged{o.pendingApplied, true});
      if (o.breakpointsChanged) emit(ev::BreakpointsChanged{});
      setState(RunState::Running);
      break;
    case StopProcessor::Outcome::AutoContinued:
      setState(RunState::Running); // auto の途中経過は公開しない
      break;
    case StopProcessor::Outcome::Stopped:
      stopInfo_ = o.info;
      if (o.imagesChanged || o.pendingApplied > 0) emit(ev::ImagesChanged{o.pendingApplied, false});
      if (o.breakpointsChanged) emit(ev::BreakpointsChanged{});
      setState(RunState::Stopped);
      emit(ev::Stopped{o.info});
      break;
  }
}

void Debugger::handleTargetExited() {
  if (!session_.isAttached()) return;
  detachSession(); // 片付け（死んだ task への操作は無害に失敗する）
  setState(RunState::Idle);
  emit(ev::TargetExited{});
}

void Debugger::handleJobDone() {
  if (!jobs_.running()) return;
  JobRunner::Outcome o = jobs_.finish();
  emit(ev::Progress{ev::Progress::Search, 0, 0, false}); // 探索の進捗表示を消す
  if (o.cancelled) finishErr(o.request, Msg::of(MsgId::ErrJobCancelled)); // ユーザが打ち切った
  else if (o.apply) o.apply(o.ok, o.value, o.error);
  // ジョブ中に保留した停止・終了を、元の順序で受信箱の先頭へ戻す。
  for (auto it = deferred_.rbegin(); it != deferred_.rend(); ++it) inbox_.pushFront(std::move(*it));
  deferred_.clear();
}

void Debugger::handleInterrupt() {
  if (auto_.interrupt()) {
    emit(ev::Notice{Msg::of(MsgId::AutoInterrupted)}); // 実行中の step が次に止まる
  } else if (state_.load() == RunState::Running && session_.isAttached()) {
    session_.stepper().interrupt();
  }
}

// ---- 共通 ----

void Debugger::setState(RunState s) {
  bool att = session_.isAttached();
  pid_t p = att ? session_.pid() : 0;
  bool changed = state_.load() != s || attached_.load() != att || pid_.load() != p;
  state_ = s; attached_ = att; pid_ = p;
  if (s != RunState::Stopped) stopInfo_ = StopInfo();
  if (changed) emit(ev::StateChanged{s, p});
}

void Debugger::finish(RequestId id, CommandResult r) {
  emit(ev::CommandFinished{id, std::move(r)});
}

void Debugger::finishOk(RequestId id, Msg m) {
  CommandResult r;
  r.ok = true; r.message = std::move(m);
  finish(id, std::move(r));
}

void Debugger::finishErr(RequestId id, Msg m) {
  CommandResult r;
  r.ok = false; r.message = std::move(m);
  finish(id, std::move(r));
}

void Debugger::detachSession() {
  // ack 待ち・join の間に来たクエリは待たせずに空を返させる。
  teardown_ = true;
  session_.detach();
  inbox_.advanceGeneration(); // これ以前に積まれた停止・終了・先読み進捗は古い対象のもの
  teardown_ = false;
  auto_.reset();
  stops_.reset();
  stopInfo_ = StopInfo();
  attached_ = false; pid_ = 0;
}

void Debugger::refreshLocked() {
  if (!session_.isAttached()) return;
  session_.refreshImages();
  session_.refreshAnnotations();
}

void Debugger::refresh() {
  if (teardown_.load() || jobs_.running()) return;
  std::lock_guard<std::mutex> lk(stateMu_);
  if (jobs_.running()) return;
  refreshLocked();
}

bool Debugger::evalSync(const AddrSpec& a, uint64_t& out, Msg& err) {
  if (a.isNumber()) { out = a.number(); return true; }
  Expression ex(session_);
  return ex.eval(a.expr(), out, err);
}

void Debugger::evalThen(RequestId id, const AddrSpec& a, EvalApply apply) {
  if (a.isNumber()) { apply(true, a.number(), Msg()); return; }
  if (!opts_.asyncEval) { // バッチ: その場で同期評価して即適用
    uint64_t v = 0; Msg e;
    bool ok = evalSync(a, v, e);
    apply(ok, v, e);
    return;
  }
  // 別スレッドで評価して UI を固めない（重い全イメージ探索は進捗つき・中止可）。
  if (!jobs_.start(id, a.expr(), std::move(apply))) finishErr(id, Msg::of(MsgId::ErrJobBusy));
}

} // namespace engine
} // namespace ta2
