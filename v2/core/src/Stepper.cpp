#include "core/Stepper.h"

#include "core/Disassembler.h"

namespace ta2 {

// RawStop → 停止理由（v1 Session::stopReason の構造化版）。
Msg stopReasonMsg(const RawStop& s) {
  if (s.syscallCatch) return Msg::of(MsgId::StopSyscallCatch).str("name", s.syscallName);
  if (s.exception == 0) return Msg(); // 通常のブレイク/ステップ停止
  switch (s.exception) {
    case EXC_BAD_ACCESS: {
      const char* kr = (s.code0 == KERN_INVALID_ADDRESS)    ? "KERN_INVALID_ADDRESS"
                     : (s.code0 == KERN_PROTECTION_FAILURE) ? "KERN_PROTECTION_FAILURE"
                     : nullptr;
      if (kr) return Msg::of(MsgId::StopBadAccess).str("kr", kr).hex("addr", (uint64_t)s.code1);
      return Msg::of(MsgId::StopBadAccessCode).num("code", s.code0).hex("addr", (uint64_t)s.code1);
    }
    case EXC_BAD_INSTRUCTION: return Msg::of(MsgId::StopBadInstruction).num("code", s.code0);
    case EXC_ARITHMETIC:      return Msg::of(MsgId::StopArithmetic).num("code", s.code0);
    default:                  return Msg::of(MsgId::StopException).num("code", s.code0);
  }
}

RawStop Stepper::rawStop() const {
  RawStop r;
  r.thread = stopThread_;
  r.pc = stopPc_;
  r.exception = stopExc_;
  r.code0 = stopCode0_;
  r.code1 = stopCode1_;
  r.watchId = lastWatchId_;
  r.syscallCatch = stopIsSyscallCatch_;
  if (stopIsSyscallCatch_) r.syscallName = scStopCaught_;
  r.imageLoad = stopIsImageLoad_;
  r.manual = manualStop_;
  return r;
}

void Stepper::notifyStop() { sink_.onRawStop(rawStop()); }

void Stepper::prepare() {
  stopState_.resize(target_.arch().threadStateCount() * sizeof(natural_t));
}

// START_SUSPENDED の時点では dyld が走る前で、まだどのイメージも読み込まれていない
//（PC は __dyld_start）。そのまま止めてもシンボルでブレイクできないので、メイン実行ファイルの
// エントリ点に一時ブレイクを置いて走らせ、dyld 完了後（全シンボル解決済み）にそこで止める。
void Stepper::armEntryStop(uint64_t entry) {
  bps_.addSoft(entry);
  tempValid_ = true; tempAddr_ = entry; // 一回限り（命中で自動除去）
  target_.resume();                     // START_SUSPENDED 分を解除 → entry で onException が止める
}

// エントリ不明（LC_UNIXTHREAD のみ等）: 現在地（dyld エントリ）で停止
void Stepper::enterInitialStop() {
  auto threads = target_.threads();
  if (!threads.empty()) {
    thread_act_t t = threads[0];
    target_.getThreadState(t, stopState_.data());
    stopPc_ = target_.arch().pc(stopState_.data());
    stopThread_ = t;
    manualStop_ = true;
    lastWatchId_ = -1;
    stopExc_ = 0;
    stopped_ = true;
    notifyStop();
  }
}

void Stepper::releaseForDetach() {
  if (stopped_ && manualStop_) {
    // 手動停止中: タスクは suspend 済み。ここで後片付けして再開する。
    bps_.removeAll();
    writeHwToThreads(); // デバッグレジスタも全消去
    setStep(stopThread_, false);
    singleStepping_ = false; rearmValid_ = false; tempValid_ = false; hwRearmIds_.clear(); watchStepId_ = -1;
    target_.resume();
    stopped_ = false; manualStop_ = false;
  } else if (stopped_) {
    // 例外スレッドにクリーンな再開を任せ、完了を待つ
    decide(Resume::Detach);
    std::unique_lock<std::mutex> lk(m_);
    ackCv_.wait(lk, [this] { return detachAck_; });
    detachAck_ = false;
  } else {
    // 実行中: こちらで一瞬止めてブレイクを全部外す
    target_.suspend();
    bps_.removeAll();
    writeHwToThreads(); // デバッグレジスタも全消去
    target_.resume();
  }
  stopIsImageLoad_ = false; // v1 Session::detach の末尾と同じ（次の対象へ内部停止フラグを持ち越さない）
}

// dtrace の stop() で既にカーネルが対象タスクを suspend 済みの状態で呼ばれる（consumer スレッド）。
void Stepper::onExternalSuspend(const std::string& caughtName) {
  std::lock_guard<std::mutex> lk(m_);
  if (!target_.isAttached()) { target_.cancelExternalSuspend(); return; }
  if (stopped_.load()) {
    // 既に別理由で停止中（タスクは自分が凍結済み）。dtrace が乗せた 1 段だけ相殺して無視する。
    target_.cancelExternalSuspend();
    return;
  }
  // dtrace が掛けた suspend を自分の計上に取り込む（continue 時の resume() 1 回で解ける）。
  target_.adoptExternalSuspend();
  // ここでは暫定で先頭スレッドを採る。発火スレッドの確定（PC シンボルと捕捉名の突き合わせ）は
  // シンボル解決を伴うので、warming スレッドとの競合を避けて決定者のスレッド側
  // （SyscallMonitor::refineStopThread → reselectStopThread）で行う。task は suspend 済みで
  // 全スレッドが凍結しているため、後から選び直しても状態は一貫する。
  auto threads = target_.threads();
  if (threads.empty()) { target_.resume(); return; } // 取り込んだ 1 段を戻して諦める
  thread_act_t t = threads[0];
  target_.getThreadState(t, stopState_.data());
  stopPc_ = target_.arch().pc(stopState_.data());
  stopThread_ = t;
  manualStop_ = true;          // 再開は Ctrl+C 停止と同じ経路（resumeManual→applyResume）
  lastWatchId_ = -1;
  stopExc_ = 0;
  stopIsSyscallCatch_ = true;
  scStopCaught_ = caughtName;
  stopped_ = true;
  notifyStop();
}

void Stepper::reselectStopThread(thread_act_t t) {
  if (!stopped_.load() || !target_.isAttached()) return;
  stopThread_ = t;
  target_.getThreadState(t, stopState_.data());
  stopPc_ = target_.arch().pc(stopState_.data());
}

void Stepper::decide(Resume r) {
  { std::lock_guard<std::mutex> lk(m_); decision_ = r; }
  resumeCv_.notify_one();
}

// 全スレッドのデバッグレジスタへ現在の HW ブレイク集合を反映。呼び出し側がタスク停止を保証する。
void Stepper::writeHwToThreads() {
  if (!target_.isAttached()) return;
  std::vector<uint8_t> ds(target_.arch().debugStateCount() * sizeof(natural_t), 0);
  for (auto t : target_.threads()) {
    if (!target_.getDebugState(t, ds.data())) continue; // 既存の SS 等を保つため read-modify-write
    bps_.applyHwInto(ds.data());
    target_.setDebugState(t, ds.data());
  }
}

void Stepper::syncHwBreaks() {
  if (!target_.isAttached()) return;
  bool needSuspend = !stopped_.load(); // 停止中はタスク凍結済み。実行中なら一瞬止める。
  if (needSuspend) target_.suspend();
  writeHwToThreads();
  if (needSuspend) target_.resume();
}

void Stepper::clearHwHitOn(thread_act_t t) {
  std::vector<uint8_t> ds(target_.arch().debugStateCount() * sizeof(natural_t), 0);
  if (!target_.getDebugState(t, ds.data())) return;
  target_.arch().clearHwHitStatus(ds.data());
  target_.setDebugState(t, ds.data());
}

// ウォッチ命中の判定。x86 は DR6 の発火ビットからスロットを得てウォッチを特定し、DR6 をクリアする。
// arm64 は debug state からは特定できないので、有効なウォッチの先頭を暫定採用する（どれが鳴ったかは不明）。
const Breakpoint* Stepper::detectWatchHit(thread_act_t thread) {
  const Breakpoint* anyEnabledWatch = nullptr;
  for (const auto& b : bps_.list()) if (b.hw && b.watch && b.enabled) { anyEnabledWatch = &b; break; }
  if (!anyEnabledWatch) return nullptr; // そもそもウォッチが無ければウォッチ命中ではない

  std::vector<uint8_t> ds(target_.arch().debugStateCount() * sizeof(natural_t), 0);
  if (!target_.getDebugState(thread, ds.data())) return anyEnabledWatch;
  int slot = target_.arch().hwBreakHit(ds.data());
  target_.arch().clearHwHitStatus(ds.data());
  target_.setDebugState(thread, ds.data()); // 発火ビットを消して戻す

  if (slot >= 0)
    for (const auto& b : bps_.list())
      if (b.hw && b.watch && b.enabled && b.slot == slot) return &b;
  return anyEnabledWatch; // スロット不明（arm64）: 暫定
}

void Stepper::interrupt() {
  if (!target_.isAttached()) return;
  std::lock_guard<std::mutex> lk(m_);
  if (stopped_.load()) return; // 既に停止中（例外や別の割り込み）
  Arch& a = target_.arch();
  target_.suspend(); // タスク全体を凍結
  auto threads = target_.threads();
  if (threads.empty()) { target_.resume(); return; }
  thread_act_t t = threads[0];
  target_.getThreadState(t, stopState_.data());
  stopPc_ = a.pc(stopState_.data());
  stopThread_ = t;
  manualStop_ = true;
  lastWatchId_ = -1;
  stopExc_ = 0;
  stopIsSyscallCatch_ = false;
  stopped_ = true;
  notifyStop();
}

// 手動停止（Ctrl+C / dtrace stop() / エントリ不明の launch）からの再開
// （例外スレッドは待機していないのでこちらで applyResume する）
void Stepper::resumeManual(Resume r) {
  stopped_ = false;
  manualStop_ = false;
  applyResume(r, stopThread_);
}

void Stepper::cont()     { if (manualStop_) resumeManual(Resume::Cont);     else decide(Resume::Cont); }
void Stepper::stepInto() { if (manualStop_) resumeManual(Resume::Step);     else decide(Resume::Step); }
void Stepper::stepOver() { if (manualStop_) resumeManual(Resume::StepOver); else decide(Resume::StepOver); }

bool Stepper::setRegister(const std::string& name, uint64_t value) {
  if (!stopped_.load()) return false;
  Arch& a = target_.arch();
  if (!a.writeRegisterByName(stopState_.data(), name, value)) return false;
  if (!target_.setThreadState(stopThread_, stopState_.data())) return false;
  stopPc_ = a.pc(stopState_.data()); // pc を書き換えた場合に表示用も合わせる
  return true;
}

void Stepper::setStep(thread_act_t t, bool on) {
  Arch& a = target_.arch();
  if (a.singleStepUsesDebugState()) {
    std::vector<uint8_t> ds(a.debugStateCount() * sizeof(natural_t), 0);
    target_.getDebugState(t, ds.data());
    a.setSingleStep(ds.data(), on);
    target_.setDebugState(t, ds.data());
  } else {
    std::vector<uint8_t> ts(a.threadStateCount() * sizeof(natural_t), 0);
    target_.getThreadState(t, ts.data());
    a.setSingleStep(ts.data(), on);
    target_.setThreadState(t, ts.data());
  }
}

kern_return_t Stepper::onException(thread_act_t thread, exception_type_t exc,
                                   const int64_t* codes, int n) {
  Arch& a = target_.arch();
  target_.suspend(); // 判断・停止中はタスク全体を凍結
  stopIsSyscallCatch_ = false; // 例外由来の停止（syscall キャッチではない）
  stopIsImageLoad_ = false;    // 既定。dyld 通知フック命中時のみ下で true

  std::vector<uint8_t> ts(a.threadStateCount() * sizeof(natural_t), 0);
  target_.getThreadState(thread, ts.data());
  uint64_t pc = a.pc(ts.data());

  // ブレイク由来でない例外（不正アドレス参照・不正命令・算術例外など）＝クラッシュ。
  // ブレイク／ステップの張り直しロジックは通さず、停止理由を添えてそのまま止める。
  // PC はフォルトした命令そのものを指す（巻き戻し不要）。
  if (exc != EXC_BREAKPOINT) {
    stopExc_ = exc;
    stopCode0_ = (n > 0) ? codes[0] : 0;
    stopCode1_ = (n > 1) ? codes[1] : 0;
    lastWatchId_ = -1;
    // またぎステップ中に別スレッドがクラッシュした等の保留があれば戻しておく
    if (rearmValid_) { bps_.addSoft(rearmAddr_); rearmValid_ = false; }
    if (!hwRearmIds_.empty()) {
      for (int id : hwRearmIds_) bps_.hwTransientEnable(id);
      hwRearmIds_.clear();
      writeHwToThreads();
    }
    if (singleStepping_) { singleStepping_ = false; setStep(thread, false); }
    stopPc_ = pc;

    target_.getThreadState(thread, stopState_.data());
    stopThread_ = thread;
    manualStop_ = false; // 例外スレッドが resume 決定を待つ
    stopped_ = true;
    notifyStop();

    Resume d;
    {
      std::unique_lock<std::mutex> lk(m_);
      resumeCv_.wait(lk, [this] { return decision_ != Resume::None; });
      d = decision_;
      decision_ = Resume::None;
    }
    stopped_ = false;
    applyResume(d, thread);
    return KERN_SUCCESS;
  }

  stopExc_ = 0; // 通常のブレイク/ステップ停止
  bool wasStepping = singleStepping_;
  lastWatchId_ = -1; // 既定。ウォッチ命中時のみ下で設定

  // SS/TF は HW ブレイクの書き戻しより先に落とす。arm64 では SS が立ったままの debug state を
  // writeHwToThreads で書き戻すと、カーネルが次の 1 命令ぶんのステップを再予約し、
  // cont でまたいだ直後に余計なステップ例外で止まる（HW 実行ブレイクの c で +4 停止）。
  if (wasStepping) { singleStepping_ = false; setStep(thread, false); }

  // 直前に外したソフトブレイクを張り直す（1 命令進んだ後）
  if (rearmValid_) { bps_.addSoft(rearmAddr_); rearmValid_ = false; }
  // またぎステップ中だけ外していた HW ブレイク/ウォッチを戻す（タスクは suspend 済み）
  if (!hwRearmIds_.empty()) {
    for (int id : hwRearmIds_) bps_.hwTransientEnable(id);
    hwRearmIds_.clear();
    writeHwToThreads();
  }

  if (wasStepping) {
    if (!stopAfterStep_) {
      // cont で bp をまたいだだけ → ユーザ停止せず走行継続
      target_.resume();
      return KERN_SUCCESS;
    }
    stopPc_ = pc; // step が着地 → ここで停止
  } else {
    // 新規のソフトブレイク #BP。PC を命令先頭へ戻す。
    uint64_t at = pc - a.softBreakPcBackup();
    const Breakpoint* hwb = bps_.userAt(pc); // HW は PC が命令そのもの（巻き戻し不要）
    if (bps_.has(at) || (tempValid_ && at == tempAddr_)) {
      a.setPc(ts.data(), at);
      target_.setThreadState(thread, ts.data());
      stopPc_ = at;
      if (tempValid_ && at == tempAddr_) { bps_.removeSoft(tempAddr_); tempValid_ = false; }
      else if (dyld_.isNotifierAddr(at) && !bps_.userAt(at))
        stopIsImageLoad_ = true; // dyld イメージ通知フック命中＝dlopen 等。上位が静かに追従して即 cont する
      else bps_.onHit(at); // ユーザブレイク命中（一時ブレイクは数えない）
    } else if (hwb && hwb->hw && !hwb->watch && hwb->enabled) {
      stopPc_ = pc;        // HW 実行ブレイク命中
      bps_.onHit(pc);
      clearHwHitOn(thread); // DR6 の発火ビットを消す（次のウォッチ判定が古いビットに惑わされないよう）
    } else if (const Breakpoint* w = detectWatchHit(thread)) {
      stopPc_ = pc;        // ウォッチポイント命中（PC はアクセス命令、または直後）
      bps_.onHit(w->addr);
      lastWatchId_ = w->id;
      // arm64 はアクセス命令で止まり再実行するので、再開時に 1 命令だけ跨ぐ必要がある。
      if (a.hwWatchStopsBeforeAccess()) watchStepId_ = w->id;
    } else {
      stopPc_ = pc; // 未知のトラップでも一応止める
    }
  }

  // ユーザに見せる停止
  target_.getThreadState(thread, stopState_.data()); // PC 補正後の状態
  stopThread_ = thread;
  manualStop_ = false; // これは例外による停止（例外スレッドが待機する）
  stopped_ = true;
  notifyStop();

  // resume 決定を待つ
  Resume d;
  {
    std::unique_lock<std::mutex> lk(m_);
    resumeCv_.wait(lk, [this] { return decision_ != Resume::None; });
    d = decision_;
    decision_ = Resume::None;
  }
  stopped_ = false;

  applyResume(d, thread);
  return KERN_SUCCESS;
}

void Stepper::applyResume(Resume d, thread_act_t thread) {
  Arch& a = target_.arch();

  // クラッシュ停止からの継続/ステップ。未解決のフォルト命令は「またげ」ない
  // （KERN_SUCCESS で再実行しても同じ所でまた落ちる）ので、例外ポートを本来のハンドラへ
  // 戻してから再実行させ、対象を通常どおりクラッシュ終了させる（＝シグナルを通す）。
  // Detach はこの後の共通処理（全ブレイク除去→resume）でクリーンに手放す。
  if (stopExc_ != 0 && d != Resume::Detach) {
    stopExc_ = 0;
    es_.restoreOriginalPorts(); // 以後この対象の例外はこちらへ来ない
    target_.resume();
    return;
  }
  stopExc_ = 0;

  // 現在 PC に張ってあるユーザブレイクを「またぐ」ための一時解除。
  //   SW: INT3 をいったん外し、1 命令後に張り直す（rearmValid_）。
  //   HW: デバッグレジスタ反映を一瞬外し、1 命令後に戻す（hwRearmIds_）。
  // どちらも「外す → 単一ステップ → 戻す」の同じ所作で、x86/arm64 共通に扱える。
  auto uncoverBreakAtPc = [&]() -> bool {
    bool covered = false;
    bool hwTouched = false;
    if (bps_.has(stopPc_)) {
      bps_.removeSoft(stopPc_);
      rearmValid_ = true; rearmAddr_ = stopPc_;
      covered = true;
    }
    const Breakpoint* ub = bps_.userAt(stopPc_);
    if (ub && ub->hw && !ub->watch && ub->enabled) { // 現在 PC の HW 実行ブレイク
      bps_.hwTransientDisable(ub->id);
      hwRearmIds_.insert(ub->id);
      hwTouched = true; covered = true;
    }
    if (watchStepId_ >= 0) { // arm64: 直前のウォッチ命中を 1 命令跨ぐ
      bps_.hwTransientDisable(watchStepId_);
      hwRearmIds_.insert(watchStepId_);
      watchStepId_ = -1;
      hwTouched = true; covered = true;
    }
    if (hwTouched) writeHwToThreads(); // タスクは suspend 済み
    return covered;
  };

  auto stepOverBreakThenRun = [&](bool stopAfter) {
    bool covered = uncoverBreakAtPc();
    if (covered || stopAfter) {
      singleStepping_ = true; stopAfterStep_ = stopAfter;
      setStep(thread, true);
    } else {
      setStep(thread, false);
    }
    target_.resume();
  };

  switch (d) {
    case Resume::Cont:
      stepOverBreakThenRun(false);
      break;

    case Resume::Step:
      // bp 上なら外して鳴らさず 1 命令、着地で停止
      uncoverBreakAtPc();
      singleStepping_ = true; stopAfterStep_ = true;
      setStep(thread, true);
      target_.resume();
      break;

    case Resume::StepOver: {
      // 現在命令を 1 つ逆アセンブルして call なら戻り先に一時 bp を置いて継続
      uint8_t code[16] = {};
      size_t got = 0;
      target_.readMemory(stopPc_, code, sizeof(code), &got);
      bps_.maskInto(stopPc_, code, got); // bp 上の call は BRK/INT3 に化けているので元命令で判定
      Disassembler dis(a);
      auto insns = dis.disasm(code, got, stopPc_, 1);
      if (!insns.empty() && insns[0].kind == InsnKind::Call) {
        tempAddr_ = stopPc_ + insns[0].size;
        tempValid_ = true;
        bps_.addSoft(tempAddr_);
        stepOverBreakThenRun(false); // 継続（戻り先 bp で止まる）
      } else {
        uncoverBreakAtPc();
        singleStepping_ = true; stopAfterStep_ = true;
        setStep(thread, true);
        target_.resume();
      }
      break;
    }

    case Resume::Detach:
      bps_.removeAll();
      writeHwToThreads(); // デバッグレジスタも全消去
      setStep(thread, false);
      singleStepping_ = false; rearmValid_ = false; tempValid_ = false; hwRearmIds_.clear(); watchStepId_ = -1;
      target_.resume();
      { std::lock_guard<std::mutex> lk(m_); detachAck_ = true; }
      ackCv_.notify_one();
      break;

    case Resume::None:
      target_.resume();
      break;
  }
}


} // namespace ta2
