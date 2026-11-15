#include "engine/StopProcessor.h"

namespace ta2 {
namespace engine {

StopProcessor::Outcome StopProcessor::process(const RawStop& raw) {
  Outcome out;
  if (!s_.isAttached() || !s_.stepper().stopped()) return out; // 古い通知（detach 済み等）

  if (raw.imageLoad && s_.stepper().stopIsImageLoad()) {
    // dlopen 等でイメージが増減した内部停止。画面には出さず（止まって見せず）、イメージ一覧を
    // 更新して、新しく載ったイメージの保留ブレイクを装着してから即 cont する。芋づる式の
    // ロードでも見える停止は 0 回。ロード直後＝そのコードを実行する前に張るので初回呼び出しも捕まえる。
    out.kind = Outcome::Internal;
    out.imagesChanged = s_.refreshImages();
    out.pendingApplied = bps_.applyPending();
    out.breakpointsChanged = out.pendingApplied > 0;
    s_.stepper().cont();
    return out;
  }

  out.imagesChanged = s_.refreshImages();
  s_.refreshAnnotations();            // ユーザ注釈を（外部編集を含め）読み直し、停止表示・BT に反映
  s_.syscalls().refineStopThread();   // syscall 停止キャッチポイントなら発火スレッドを確定（複数スレッド対応）
  s_.warmSymbols();                   // 初回停止でイメージが揃ったら裏でシンボルを先読み（冪等）
  if (opts_.dyldNotifier) s_.dyld().ensure(); // dlopen 追従フックを設置（冪等。dyld 初期化後＝この停止で有効）

  AutoRunner::Verdict av = auto_.onStop(s_);
  if (av.keepGoing) {
    s_.stepper().stepInto(); // まだ興味なし → さらに 1 命令
    out.kind = Outcome::AutoContinued;
    return out;
  }

  out.kind = Outcome::Stopped;
  out.info = buildStopInfo(av);

  // 起動直後の確定停止（エントリ）で、保存ブレイクを一度だけ復元する（images が揃ったこの時点で）。
  if (restorePending_) {
    restorePending_ = false;
    int n = bps_.restoreSaved();
    if (n > 0) { out.info.notes.push_back(Msg::of(MsgId::BpRestored).num("n", n)); out.breakpointsChanged = true; }
    if (bps_.pendingCount() > 0)
      out.info.notes.push_back(Msg::of(MsgId::BpRestorePending).num("n", (int64_t)bps_.pendingCount()));
  }
  // dlopen 等で後からロードされたイメージの保留ブレイクを、イメージが揃ったこの停止で張り直す。
  int pend = bps_.applyPending();
  if (pend > 0) {
    out.info.notes.push_back(Msg::of(MsgId::BpRestoredNewLoad).num("n", pend));
    out.pendingApplied = pend;
    out.breakpointsChanged = true;
  }
  return out;
}

StopInfo StopProcessor::buildStopInfo(const AutoRunner::Verdict& av) {
  // refineStopThread で停止スレッド / PC が確定した後の値を使う（積まれた RawStop は暫定のことがある）。
  RawStop cur = s_.stepper().rawStop();
  StopInfo info;
  info.pc = cur.pc;
  info.thread = (ThreadId)cur.thread;
  info.where = s_.describe(cur.pc);
  SourceLoc loc;
  if (s_.sourceLineFor(cur.pc, loc)) info.source = loc;

  const Breakpoint* ub = s_.breaks().userAt(cur.pc);
  bool bpHit = ub && ub->enabled;
  if (bpHit) { info.bpId = ub->id; info.bpHits = ub->hits; info.bpHw = ub->hw; }
  if (cur.watchId >= 0) {
    if (const Breakpoint* wb = s_.breaks().byId(cur.watchId)) {
      info.watchId = cur.watchId; info.watchAddr = wb->addr; info.watchHits = wb->hits;
    }
  }
  info.exception = (int)cur.exception;
  info.code0 = cur.code0;
  info.faultAddr = (uint64_t)cur.code1;
  if (cur.syscallCatch) info.syscallCaught = cur.syscallName;
  info.reason = stopReasonMsg(cur);
  SyscallCall sc;
  if (s_.syscallAtStop(sc)) info.syscall = sc;
  info.autoStop = av.why;
  info.autoSteps = av.steps;

  if (cur.exception != 0)             info.kind = StopKind::Exception;
  else if (cur.syscallCatch)          info.kind = StopKind::SyscallCatch;
  else if (info.watchId >= 0)         info.kind = StopKind::Watchpoint;
  else if (bpHit)                     info.kind = StopKind::Breakpoint;
  else if (av.wasAuto)                info.kind = StopKind::Auto;
  else if (entryPending_)             info.kind = StopKind::Entry;
  else if (cur.manual)                info.kind = StopKind::Interrupt;
  else if (lastResume_ == Resume::StepInto || lastResume_ == Resume::StepOver) info.kind = StopKind::Step;
  else                                info.kind = StopKind::Unknown;
  entryPending_ = false;
  return info;
}

} // namespace engine
} // namespace ta2
