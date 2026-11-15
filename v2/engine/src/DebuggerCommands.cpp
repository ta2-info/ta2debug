// Debugger のコマンド実体。v1 main.cpp の runCommand / doAttach / doLaunch / doRestart /
// toggleBreakAtCursor / annotateAtCursor / saveBreakpoints / ブレイク編集の中核を、
// g_message → CommandResult（Msg）に置き換えて移したもの。判定順・既定値は v1 のまま。
#include "engine/Debugger.h"

#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

#include <sys/wait.h>

#include "core/MemoryExaminer.h"

namespace ta2 {
namespace engine {

namespace {

// そのイメージ（バイナリ／dylib）のオフセットを 16 進で表すのに要る桁数（v1 imageHexDigits）。
// __TEXT の静的先頭（vmaddr）を基準に、全セグメントの終端の最大までを「イメージのサイズ」とみなす。
int imageHexDigits(const Image& im) {
  uint64_t base = im.loadAddr - im.slide; // __TEXT vmaddr（静的）
  uint64_t end = im.textSize;             // 最低でも __TEXT 分
  for (const auto& s : im.segments) {
    if (s.vmaddr < base) continue;        // __PAGEZERO など基準より前は無視
    uint64_t e = (s.vmaddr - base) + s.vmsize;
    if (e > end) end = e;
  }
  uint64_t maxOff = end ? end - 1 : 0;
  int d = 0; do { d++; maxOff >>= 4; } while (maxOff);
  return d;
}

} // namespace

// ---- 対象の管理 ----

void Debugger::exec(RequestId id, cmd::Attach& c) {
  if (session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrAlreadyAttached)); return; }
  Msg err;
  if (session_.attach(c.pid, err)) {
    stops_.noteAttach();
    setState(RunState::Running);
    finishOk(id, Msg::of(MsgId::Attached).num("pid", c.pid));
  } else {
    finishErr(id, Msg::of(MsgId::ErrAttachFailed).sub("error", err));
  }
}

// バイナリを起動してデバッグ開始。args[0]=実行ファイルパス、以降は対象への引数。
// restoreSaved=true なら、永続化済みブレイクをエントリ停止後に自動で張り直す（設定で無効化可）。
bool Debugger::doLaunch(const std::vector<std::string>& args, bool restoreSaved, Msg& msg) {
  if (args.empty() || args[0].empty()) { msg = Msg::of(MsgId::ErrLaunchNoPath); return false; }
  std::vector<char*> cargv;
  for (const auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);
  Msg err;
  if (!session_.launch(args[0], cargv.data(), err)) {
    msg = Msg::of(MsgId::ErrLaunchFailed).sub("error", err);
    return false;
  }
  launchArgs_ = args; // 再起動（:restart）で同じ引数・同じアドレスで起動し直せるよう控える
  // 保存ブレイクの復元は dyld 完了後（エントリ停止）に行う。ここでは予約だけ。
  stops_.noteLaunch(restoreSaved && opts_.autoRestoreBreakpoints);
  // エントリへ走行中→Running、停止済み→Stopped（停止の中身は続く RawStop の処理で確定する）
  setState(session_.stepper().stopped() ? RunState::Stopped : RunState::Running);
  msg = Msg::of(MsgId::Launched).str("path", args[0]);
  return true;
}

void Debugger::exec(RequestId id, cmd::Launch& c) {
  if (session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrAlreadyLaunched)); return; }
  Msg msg;
  if (doLaunch(c.args, /*restoreSaved=*/true, msg)) finishOk(id, msg);
  else finishErr(id, msg);
}

// 対象を終了させて、同じ引数・同じアドレス（ASLR 無効で slide=0）で起動し直す。
// ユーザが張ったブレイクは控えておき、再起動後に同一アドレスへ張り直す（種別・有効状態も保つ）。
void Debugger::exec(RequestId id, cmd::Restart&) {
  if (launchArgs_.empty()) { finishErr(id, Msg::of(MsgId::ErrRestartNotLaunched)); return; }
  std::vector<Breakpoint> saved = session_.breaks().list(); // 再起動後に張り直すユーザブレイク
  pid_t oldpid = session_.isAttached() ? session_.pid() : 0;
  if (session_.isAttached()) detachSession(); // 自分で起動した子は Target::detach が SIGKILL する
  session_.breaks().removeAll();              // 旧 task 向けのユーザ一覧を一掃
  bps_.clearPending();                        // 保留中の dlopen 待ちブレイクも破棄（再起動は自前で引き継ぐ）
  if (oldpid > 0) { int st; while (waitpid(oldpid, &st, 0) < 0 && errno == EINTR) {} } // ゾンビ防止
  setState(RunState::Idle);

  auto args = launchArgs_; // doLaunch が launchArgs_ を書き換えるので退避
  Msg msg;
  if (!doLaunch(args, /*restoreSaved=*/false, msg)) { // 控えたブレイクを自分で張り直すのでファイル復元はしない
    emit(ev::BreakpointsChanged{});
    finishErr(id, msg);
    return;
  }
  int reapplied = bps_.reapply(saved);
  emit(ev::BreakpointsChanged{});
  if (saved.empty()) finishOk(id, Msg::of(MsgId::Restarted).str("path", args[0]));
  else finishOk(id, Msg::of(MsgId::RestartedWithBps).str("path", args[0]).num("n", reapplied)
                      .num("total", (int64_t)saved.size()));
}

void Debugger::exec(RequestId id, cmd::Detach&) {
  bool had = session_.isAttached();
  detachSession();
  setState(RunState::Idle);
  if (had) emit(ev::BreakpointsChanged{}); // detach はユーザ一覧も消す
  finishOk(id, Msg::of(MsgId::Detached));
}

// ---- 実行制御 ----

void Debugger::resume(RequestId id, StopProcessor::Resume how) {
  if (!stoppedLocked()) { finishErr(id, Msg::of(MsgId::ErrNotStopped)); return; }
  stops_.noteResume(how);
  if (how == StopProcessor::Resume::StepInto)      session_.stepper().stepInto();
  else if (how == StopProcessor::Resume::StepOver) session_.stepper().stepOver();
  else                                             session_.stepper().cont();
  setState(RunState::Running);
  finishOk(id);
}

void Debugger::exec(RequestId id, cmd::Continue&) { resume(id, StopProcessor::Resume::Continue); }
void Debugger::exec(RequestId id, cmd::StepInto&) { resume(id, StopProcessor::Resume::StepInto); }
void Debugger::exec(RequestId id, cmd::StepOver&) { resume(id, StopProcessor::Resume::StepOver); }

void Debugger::exec(RequestId id, cmd::Auto& c) {
  if (!stoppedLocked()) { finishErr(id, Msg::of(MsgId::ErrAutoNeedsStop)); return; }
  unsigned mask = c.mask & 7u;
  if (!mask) { finishErr(id, Msg::of(MsgId::ErrAutoBadArg)); return; }
  auto_.start(mask);
  stops_.noteResume(StopProcessor::Resume::StepInto);
  session_.stepper().stepInto(); // 以降は StopProcessor が自動継続
  setState(RunState::Running);
  finishOk(id, Msg::of(MsgId::AutoRunning));
}

// ---- ブレイクポイント ----

void Debugger::exec(RequestId id, cmd::AddBreakpoint& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  BpKind kind = c.kind;
  WatchParams w = c.watch;
  if (kind == BpKind::Watch) {
    if (w.size != 1 && w.size != 2 && w.size != 4 && w.size != 8) {
      finishErr(id, Msg::of(MsgId::ErrWatchBadArg).str("arg", std::to_string(w.size)));
      return;
    }
    if (!w.onRead && !w.onWrite) w.onWrite = true; // 既定は書込監視
  }
  evalThen(id, c.at, [this, id, kind, w](bool ok, uint64_t addr, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    CommandResult r;
    r.value = addr;
    if (kind == BpKind::Soft) {
      const Breakpoint* bp = session_.breaks().add(addr);
      if (!bp) { finishErr(id, Msg::of(MsgId::ErrBpSetFailed)); return; }
      r.ok = true; r.breakpointId = bp->id;
      r.message = Msg::of(MsgId::BpSet).num("id", bp->id).hex("addr", addr);
    } else if (kind == BpKind::HwExec) {
      const Breakpoint* bp = session_.breaks().addHw(addr);
      if (!bp) { finishErr(id, Msg::of(MsgId::ErrBpHwFull).num("max", session_.breaks().hwCapacity())); return; }
      session_.syncHwBreaks(); // デバッグレジスタへ反映
      r.ok = true; r.breakpointId = bp->id;
      r.message = Msg::of(MsgId::BpHwSet).num("id", bp->id).hex("addr", addr);
    } else {
      if (addr % (uint64_t)w.size != 0) {
        finishErr(id, Msg::of(MsgId::ErrWatchUnaligned).hex("addr", addr).num("size", w.size));
        return;
      }
      const Breakpoint* wp = session_.breaks().addWatch(addr, w.size, w.onRead, w.onWrite);
      if (!wp) { finishErr(id, Msg::of(MsgId::ErrWatchFull).num("max", session_.breaks().watchCapacity())); return; }
      session_.syncHwBreaks();
      const char* mode = (w.onRead && w.onWrite) ? "R/W" : w.onRead ? "R" : "W";
      r.ok = true; r.breakpointId = wp->id;
      r.message = Msg::of(MsgId::WatchSet).num("id", wp->id).hex("addr", addr).num("size", w.size).str("mode", mode);
    }
    r.where = session_.describe(addr);
    emit(ev::BreakpointsChanged{});
    finish(id, std::move(r));
  });
}

// 「#番号」（int）ならその場で、アドレス（式可）なら評価してからブレイク id を fn に渡す（v1 withBreakId）。
void Debugger::withBreakId(RequestId id, const BpRef& ref, std::function<void(int)> fn) {
  if (std::holds_alternative<int>(ref.v)) { fn(std::get<int>(ref.v)); return; }
  evalThen(id, std::get<AddrSpec>(ref.v), [this, id, fn](bool ok, uint64_t addr, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    const Breakpoint* b = session_.breaks().userAt(addr);
    if (!b) { finishErr(id, Msg::of(MsgId::ErrBpNoneAtAddr)); return; }
    fn(b->id);
  });
}

void Debugger::exec(RequestId id, cmd::RemoveBreakpoint& c) {
  withBreakId(id, c.ref, [this, id](int bid) {
    if (!bps_.remove(bid)) { finishErr(id, Msg::of(MsgId::ErrBpNotFound)); return; }
    CommandResult r;
    r.ok = true; r.breakpointId = bid;
    r.message = Msg::of(MsgId::BpRemoved).num("id", bid);
    emit(ev::BreakpointsChanged{});
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::ToggleEnabled& c) {
  withBreakId(id, c.ref, [this, id](int bid) {
    bool now = false, found = false;
    if (!bps_.toggleEnabled(bid, now, found)) {
      finishErr(id, Msg::of(found ? MsgId::ErrBpToggleFailed : MsgId::ErrBpNotFound));
      return;
    }
    CommandResult r;
    r.ok = true; r.breakpointId = bid;
    r.message = Msg::of(now ? MsgId::BpEnabled : MsgId::BpDisabled).num("id", bid);
    emit(ev::BreakpointsChanged{});
    finish(id, std::move(r));
  });
}

// b キー / クリック: そのアドレスのブレイクをトグル（有れば解除 / 無ければ SW ブレイク設定）。
void Debugger::exec(RequestId id, cmd::ToggleBreakpointAt& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  uint64_t a = c.addr;
  CommandResult r;
  r.value = a;
  const Breakpoint* ex = session_.breaks().userAt(a);
  if (ex) {
    int bid = ex->id; bool hw = ex->hw;
    if (!session_.breaks().removeId(bid)) { finishErr(id, Msg::of(MsgId::ErrBpClearFailed)); return; }
    if (hw) session_.syncHwBreaks();
    r.ok = true; r.breakpointId = bid;
    r.message = Msg::of(MsgId::BpCleared).num("id", bid).hex("addr", a);
  } else {
    const Breakpoint* bp = session_.breaks().add(a);
    if (!bp) { finishErr(id, Msg::of(MsgId::ErrBpSetFailed)); return; }
    r.ok = true; r.breakpointId = bp->id;
    r.message = Msg::of(MsgId::BpSet).num("id", bp->id).hex("addr", a);
    r.where = session_.describe(a);
  }
  emit(ev::BreakpointsChanged{});
  finish(id, std::move(r));
}

void Debugger::exec(RequestId id, cmd::RebuildBreakpoint& c) {
  const Breakpoint* old = session_.breaks().byId(c.id);
  if (!old) { finishErr(id, Msg::of(MsgId::ErrBpNotFound)); return; }
  int newId = -1;
  bool ok = bps_.rebuild(*old, c.kind, c.addr, c.watch.size, c.watch.onRead, c.watch.onWrite, newId);
  CommandResult r;
  r.ok = ok;
  if (newId >= 0) r.breakpointId = newId; // 成功: 新しい id ／ 失敗: 巻き戻して張り直した id
  r.value = c.addr;
  if (!ok) r.message = Msg::of(MsgId::ErrBpRebuildFailed);
  emit(ev::BreakpointsChanged{});
  finish(id, std::move(r));
}

void Debugger::exec(RequestId id, cmd::SaveBreakpoints&) {
  Msg msg;
  if (bps_.save(msg)) finishOk(id, msg);
  else finishErr(id, msg);
}

// ---- 書き換え ----

void Debugger::exec(RequestId id, cmd::SetRegister& c) {
  if (!stoppedLocked()) { finishErr(id, Msg::of(MsgId::ErrRegNeedsStop)); return; }
  std::string name = c.name;
  evalThen(id, c.value, [this, id, name](bool ok, uint64_t val, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    if (!stoppedLocked()) { finishErr(id, Msg::of(MsgId::ErrRegNeedsStop)); return; } // 評価中に状態が変わった場合
    if (!session_.stepper().setRegister(name, val)) {
      finishErr(id, Msg::of(MsgId::ErrRegNotWritable).str("name", name));
      return;
    }
    if (state_.load() == RunState::Stopped) { // pc を書き換えた場合に直近の停止情報も合わせる
      stopInfo_.pc = session_.stepper().stopPc();
      stopInfo_.where = session_.describe(stopInfo_.pc);
    }
    CommandResult r;
    r.ok = true; r.value = val;
    r.message = Msg::of(MsgId::RegSet).str("name", name).hex("value", val);
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::WriteMemory& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  std::vector<uint8_t> bytes = c.bytes;
  evalThen(id, c.at, [this, id, bytes](bool ok, uint64_t addr, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    if (!session_.target().writeMemory(addr, bytes.data(), bytes.size())) {
      finishErr(id, Msg::of(MsgId::ErrMemWriteFailed));
      return;
    }
    CommandResult r;
    r.ok = true; r.value = addr;
    r.message = Msg::of(MsgId::MemWritten).hex("addr", addr).num("n", (int64_t)bytes.size());
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::WriteValue& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  // 値側は先にその場で解釈し、アドレス式だけ非同期評価する（v1 と同じ）。
  uint64_t val = 0; Msg eerr;
  if (!evalSync(c.value, val, eerr)) { finishErr(id, exprError(eerr)); return; }
  int size = c.size;
  if (size < 1) size = 1;
  if (size > 8) size = 8;
  cmd::WriteMemory w;
  w.at = c.at;
  for (int i = 0; i < size; i++) w.bytes.push_back((uint8_t)((val >> (8 * i)) & 0xff)); // リトルエンディアン
  exec(id, w);
}

void Debugger::exec(RequestId id, cmd::FillMemory& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  // 値・サイズは先にその場で解釈し、アドレス式だけ非同期評価する。
  uint64_t val = 0, size = 0; Msg eerr;
  if (!evalSync(c.value, val, eerr) || !evalSync(c.size, size, eerr)) { finishErr(id, exprError(eerr)); return; }
  if (size == 0 || size > (1u << 24)) { finishErr(id, Msg::of(MsgId::ErrMemFillSize)); return; }
  evalThen(id, c.at, [this, id, val, size](bool ok, uint64_t addr, const Msg& e2) {
    if (!ok) { finishErr(id, exprError(e2)); return; }
    std::vector<uint8_t> buf((size_t)size, (uint8_t)(val & 0xff));
    if (!session_.target().writeMemory(addr, buf.data(), buf.size())) {
      finishErr(id, Msg::of(MsgId::ErrMemFillFailed));
      return;
    }
    char vb[16]; snprintf(vb, sizeof(vb), "0x%02x", (unsigned)(val & 0xff));
    CommandResult r;
    r.ok = true; r.value = addr;
    r.message = Msg::of(MsgId::MemFilled).hex("addr", addr).str("value", vb).num("size", (int64_t)size);
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::DumpToFile& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  uint64_t size = 0; Msg eerr;
  if (!evalSync(c.size, size, eerr)) { finishErr(id, exprError(eerr)); return; }
  std::string path = c.path;
  if (path.empty() || size == 0 || size > (1u << 28)) { finishErr(id, Msg::of(MsgId::ErrFileArgs)); return; }
  evalThen(id, c.at, [this, id, size, path](bool ok, uint64_t addr, const Msg& e2) {
    if (!ok) { finishErr(id, exprError(e2)); return; }
    std::vector<uint8_t> buf((size_t)size); size_t got = 0; // 対象メモリ → ファイル
    if (!session_.target().readMemory(addr, buf.data(), buf.size(), &got) || got == 0) {
      finishErr(id, Msg::of(MsgId::ErrMemReadFailed));
      return;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { finishErr(id, Msg::of(MsgId::ErrFileOpen).str("path", path)); return; }
    size_t wrote = fwrite(buf.data(), 1, got, f); fclose(f);
    CommandResult r;
    r.ok = true; r.value = addr;
    r.message = Msg::of(MsgId::FileDumped).hex("addr", addr).num("n", (int64_t)wrote).str("path", path);
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::LoadFromFile& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  uint64_t size = 0; Msg eerr;
  if (!evalSync(c.size, size, eerr)) { finishErr(id, exprError(eerr)); return; }
  std::string path = c.path;
  if (path.empty() || size == 0 || size > (1u << 28)) { finishErr(id, Msg::of(MsgId::ErrFileArgs)); return; }
  evalThen(id, c.at, [this, id, size, path](bool ok, uint64_t addr, const Msg& e2) {
    if (!ok) { finishErr(id, exprError(e2)); return; }
    FILE* f = fopen(path.c_str(), "rb"); // ファイル → 対象メモリ
    if (!f) { finishErr(id, Msg::of(MsgId::ErrFileOpen).str("path", path)); return; }
    std::vector<uint8_t> buf((size_t)size); size_t got = fread(buf.data(), 1, buf.size(), f); fclose(f);
    if (got == 0) { finishErr(id, Msg::of(MsgId::ErrFileEmpty)); return; }
    if (!session_.target().writeMemory(addr, buf.data(), got)) {
      finishErr(id, Msg::of(MsgId::ErrMemLoadFailed));
      return;
    }
    CommandResult r;
    r.ok = true; r.value = addr;
    r.message = Msg::of(MsgId::FileLoaded).str("path", path).num("n", (int64_t)got).hex("addr", addr);
    finish(id, std::move(r));
  });
}

// ---- 注釈 / 既定ライブラリ ----

void Debugger::exec(RequestId id, cmd::SetAnnotation& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  bool hasName = c.name.has_value();
  std::string name = c.name.value_or(std::string());
  std::string memo = c.memo.value_or(std::string());
  if (hasName && name.empty()) { finishErr(id, Msg::of(MsgId::ErrAnnEmptyName)); return; }
  evalThen(id, c.at, [this, id, hasName, name, memo](bool ok, uint64_t addr, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    const Image* im = session_.images().byAddress(addr);
    if (!im) { finishErr(id, Msg::of(MsgId::ErrAnnNoImage)); return; }
    Msg err;
    CommandResult r;
    r.value = addr;
    std::string path = Annotations::pathFor(*im);
    if (hasName) { // :ann <式>,<名前>[,<メモ>]
      bool good = session_.annotations().setName(*im, addr, name, err);
      if (good && !memo.empty()) good = session_.annotations().setMemo(*im, addr, memo, err);
      if (!good) { finishErr(id, Msg::of(MsgId::ErrAnnSave).sub("error", err)); return; }
      r.message = memo.empty()
        ? Msg::of(MsgId::AnnSet).hex("addr", addr).str("name", name).str("path", path)
        : Msg::of(MsgId::AnnSetMemo).hex("addr", addr).str("name", name).str("memo", memo).str("path", path);
    } else {       // :memo <式>,<メモ>（空メモで削除）
      if (!session_.annotations().setMemo(*im, addr, memo, err)) {
        finishErr(id, Msg::of(MsgId::ErrMemoSave).sub("error", err));
        return;
      }
      r.message = memo.empty()
        ? Msg::of(MsgId::MemoRemoved).hex("addr", addr).str("path", path)
        : Msg::of(MsgId::MemoSet).hex("addr", addr).str("memo", memo).str("path", path);
    }
    r.ok = true;
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::RemoveAnnotation& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  evalThen(id, c.at, [this, id](bool ok, uint64_t addr, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    const Image* im = session_.images().byAddress(addr);
    if (!im) { finishErr(id, Msg::of(MsgId::ErrAnnNoImage)); return; }
    Msg err;
    if (!session_.annotations().remove(*im, addr, err)) { finishErr(id, err); return; }
    CommandResult r;
    r.ok = true; r.value = addr;
    r.message = Msg::of(MsgId::AnnRemoved).hex("addr", addr);
    finish(id, std::move(r));
  });
}

// m キー: そのアドレスにユーザシンボル（注釈）を即時登録する（v1 annotateAtCursor）。
//  - 未登録なら func<0詰16進オフセット>（桁数はイメージサイズから算出）で名前を付けて保存。
//  - 既に登録済みなら変更せず、既存の名前／メモを返す（表示側が :ann の編集行を開く）。
void Debugger::exec(RequestId id, cmd::AutoAnnotate& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  uint64_t a = c.addr;
  const Image* im = session_.images().byAddress(a);
  if (!im) { finishErr(id, Msg::of(MsgId::ErrAnnNoImage)); return; }
  std::string path = Annotations::pathFor(*im);
  if (path.empty()) { finishErr(id, Msg::of(MsgId::ErrAnnNoUuidImage).str("image", im->name)); return; }
  session_.annotations().refresh(*im); // 外部編集を取り込んでから判定
  CommandResult r;
  r.ok = true; r.value = a;
  AnnotationEdit ed;
  ed.addr = a;
  if (const Annot* ex = session_.annotations().atExact(*im, a)) {
    ed.existing = true; ed.name = ex->name; ed.memo = ex->memo;
    r.payload = ed;
    finish(id, std::move(r));
    return;
  }
  uint64_t off = a - im->loadAddr; // イメージ先頭（__TEXT）からのオフセット
  int digits = imageHexDigits(*im);
  char nm[40]; snprintf(nm, sizeof(nm), "func%0*llx", digits, (unsigned long long)off);
  Msg err;
  if (!session_.annotations().setName(*im, a, nm, err)) {
    finishErr(id, Msg::of(MsgId::ErrAnnRegister).sub("error", err));
    return;
  }
  ed.name = nm;
  r.payload = ed;
  r.message = Msg::of(MsgId::AnnRegistered).str("name", nm).hex("addr", a).str("image", im->name)
                .hex("off", off).str("path", path);
  finish(id, std::move(r));
}

void Debugger::exec(RequestId id, cmd::SetDefaultLib& c) {
  if (c.name.empty()) {
    session_.setDefaultLib("");
    finishOk(id, Msg::of(MsgId::DefaultLibCleared));
    return;
  }
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  const Image* im = session_.images().byName(c.name);
  if (!im) { finishErr(id, Msg::of(MsgId::ErrLibNotFound).str("name", c.name)); return; }
  session_.setDefaultLib(c.name);
  finishOk(id, Msg::of(MsgId::DefaultLibSet).str("name", im->name));
}

// ---- syscall 監視 ----

void Debugger::exec(RequestId id, cmd::SyscallWatch& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  bool all = c.filter.empty() || c.filter == "all";
  std::string filter = all ? std::string() : c.filter; // "" = 全件
  Msg err;
  if (!session_.syscalls().startWatch(filter, err)) {
    finishErr(id, Msg::of(MsgId::ErrScWatchStart).sub("error", err));
    return;
  }
  bool tr = session_.syscalls().traceMode();
  finishOk(id, Msg::of(tr ? MsgId::ScTraceStarted : MsgId::ScWatchStarted)
                 .str("filter", all ? std::string("all") : c.filter));
}

void Debugger::exec(RequestId id, cmd::SyscallWatchOff&) {
  // いま稼働中の dtrace ハンドルが :scstop（停止キャッチポイント）なら、:scwatch からは触らない。
  bool stopActive = session_.syscalls().active() && session_.syscalls().stopMode();
  if (stopActive) { finishErr(id, Msg::of(MsgId::ErrScWatchOffIsStop)); return; }
  if (!session_.syscalls().active()) { finishErr(id, Msg::of(MsgId::ErrScWatchNotRunning)); return; }
  session_.syscalls().stop();
  finishOk(id, Msg::of(MsgId::ScWatchStopped));
}

void Debugger::exec(RequestId id, cmd::SyscallStop& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  bool all = c.filter.empty() || c.filter == "all";
  std::string filter = all ? std::string() : c.filter; // "" = 全件
  Msg err;
  if (!session_.syscalls().startStop(filter, err)) {
    finishErr(id, Msg::of(MsgId::ErrScStopStart).sub("error", err));
    return;
  }
  finishOk(id, Msg::of(MsgId::ScStopSet).str("filter", all ? std::string("all") : c.filter));
}

void Debugger::exec(RequestId id, cmd::SyscallStopOff&) {
  if (!(session_.syscalls().active() && session_.syscalls().stopMode())) {
    finishErr(id, Msg::of(MsgId::ErrScStopNotSet));
    return;
  }
  session_.syscalls().stop();
  finishOk(id, Msg::of(MsgId::ScStopCleared));
}

// ---- 参照（式評価を伴うもの） ----

void Debugger::exec(RequestId id, cmd::Evaluate& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  evalThen(id, AddrSpec(c.expr), [this, id](bool ok, uint64_t v, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    CommandResult r;
    r.ok = true; r.value = v;
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::Describe& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  evalThen(id, c.at, [this, id](bool ok, uint64_t addr, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    CommandResult r;
    r.ok = true; r.value = addr;
    r.where = session_.describe(addr);
    if (!r.where->hasImage) r.message = Msg::of(MsgId::SymNoImage);
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::Examine& c) {
  if (!session_.isAttached()) { finishErr(id, Msg::of(MsgId::ErrNeedAttach)); return; }
  std::string spec = c.spec;
  int bytes = c.classicBytes;
  evalThen(id, c.at, [this, id, spec, bytes](bool ok, uint64_t addr, const Msg& eerr) {
    if (!ok) { finishErr(id, exprError(eerr)); return; }
    MemoryExaminer ex(session_.target(), session_.breaks(), intel_.load());
    ExamineResult res; Msg err;
    bool good = spec.empty() ? ex.classicDump(addr, bytes, res, err)  // 従来の 16 進ダンプ（バイト数指定可）
                             : ex.examine(spec, addr, res, err);
    if (!good) { finishErr(id, err); return; }
    CommandResult r;
    r.ok = true; r.value = addr;
    r.payload = std::move(res);
    finish(id, std::move(r));
  });
}

void Debugger::exec(RequestId id, cmd::SetDisasmSyntax& c) {
  intel_ = c.intel; // 以降に生成される逆アセンブラへ効く（x86 のみ）
  finishOk(id, Msg::of(MsgId::SyntaxSet).str("name", c.intel ? "Intel" : "AT&T"));
}

} // namespace engine
} // namespace ta2
