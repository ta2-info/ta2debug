// L3 Presentation: ta2tui — 対話 TUI 正面。v1 の TUI を v2 ヘッドレス Debugger の上へ写した第一カット。
// エンジンは threaded=true（専用スレッドが受信箱を処理）。イベントは IEventSink で来る（エンジンスレッド）
// ので self-pipe でメインスレッドを起こし、メインは Debugger のクエリを引き直して再描画する。入力は
// 同じ poll で拾う。描画は Terminal（代替画面・raw）＋ TextReport。コマンド解釈は CommandInterpreter(Tui)。
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include "engine/CommandInterpreter.h"
#include "engine/Debugger.h"
#include "engine/Event.h"
#include "core/SymbolRef.h" // formatDescribe（ヘッダの @where 表示）
#include "i18n/Translator.h"
#include "ui/Terminal.h"
#include "ui/TextReport.h"

using namespace ta2;
using namespace ta2::engine;

namespace {

int g_wakePipe[2] = {-1, -1}; // エンジンイベント／SIGWINCH でメインの poll を起こす
void wake() { if (g_wakePipe[1] >= 0) { char c = 'x'; ssize_t n = write(g_wakePipe[1], &c, 1); (void)n; } }
void onWinch(int) { wake(); }

// エンジンイベントを受けてメインを起こす。結果メッセージ・停止/終了・通知を溜める（排他）。
struct TuiSink : IEventSink {
  std::mutex mu;
  std::vector<std::pair<RequestId, CommandResult>> finished;
  std::vector<Msg> notices;
  bool exited = false, justStopped = false;

  void onEvent(const DebugEvent& ev) override {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (auto e = std::get_if<ev::CommandFinished>(&ev)) finished.push_back({e->request, e->result});
      else if (auto n = std::get_if<ev::Notice>(&ev)) notices.push_back(n->message);
      else if (std::get_if<ev::TargetExited>(&ev)) exited = true;
      else if (std::get_if<ev::Stopped>(&ev)) justStopped = true;
      // StateChanged / BreakpointsChanged / ImagesChanged / Progress は再クエリで拾う
    }
    wake();
  }
};

class TuiApp {
public:
  TuiApp() : dbg_(EngineOptions::interactive()), tr_(loadCatalog(detectLocaleLang()), detectLocaleLang()),
             interp_(dbg_), report_(tr_, dbg_) {
    sub_ = dbg_.subscribe(&sink_);
  }
  ~TuiApp() { dbg_.unsubscribe(sub_); }

  int run(const std::vector<std::string>& initial) {
    term_.enter();
    for (const auto& l : initial) runLine(l);
    render();
    while (!quit_) {
      struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {g_wakePipe[0], POLLIN, 0}};
      int r = poll(fds, 2, 300);
      if (r < 0) { if (errno == EINTR) continue; break; }
      if (fds[1].revents & POLLIN) { char b[256]; while (read(g_wakePipe[0], b, sizeof(b)) > 0) {} }
      if (fds[0].revents & POLLIN) readKeys();
      drainEvents();
      term_.refreshSize();
      render();
    }
    term_.leave();
    return 0;
  }

private:
  // ---- 入力 ----
  void readKeys() {
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    std::string in; char b[512]; ssize_t n;
    while ((n = read(STDIN_FILENO, b, sizeof(b))) > 0) in.append(b, n);
    fcntl(STDIN_FILENO, F_SETFL, fl);
    for (size_t i = 0; i < in.size();) {
      if (in[i] == 0x1b && i + 2 < in.size() && in[i+1] == '[') {
        char c = in[i+2];
        if (c == 'A') { key(KeyUp); i += 3; continue; }
        if (c == 'B') { key(KeyDown); i += 3; continue; }
        if (c == 'C') { key(KeyRight); i += 3; continue; }
        if (c == 'D') { key(KeyLeft); i += 3; continue; }
        if (c == '<') { // SGR マウス: \x1b[<b;x;y(M|m)
          size_t j = i + 3; int b = 0, mx = 0, my = 0; auto num = [&](int& v){ v = 0; while (j < in.size() && in[j] >= '0' && in[j] <= '9') v = v*10 + (in[j++]-'0'); };
          num(b); if (j < in.size() && in[j]==';') j++; num(mx); if (j < in.size() && in[j]==';') j++; num(my);
          char tail = (j < in.size()) ? in[j] : 'M'; j++;
          int mcol = mx - 1, mrow = my - 1;
          if (tail == 'M' && (b & 0x3) == 0 && !(b & 0x20)) mouseDown(mcol, mrow); // 左押下
          else if (tail == 'M' && (b & 0x20)) mouseDrag(mcol, mrow);              // ドラッグ（移動）
          else if (tail == 'm') mouseUp(mcol, mrow);                              // 離す
          i = j; continue;
        }
      }
      if (in[i] == 0x1b) { key(KeyEsc); i++; continue; }
      key((unsigned char)in[i]); i++;
    }
  }

  enum { KeyUp = 0x1000, KeyDown, KeyLeft, KeyRight, KeyEsc = 0x1b };

  // マウス左押下: 選択開始を記録（離すときにドラッグなら範囲コピー、無ければ行フォーカス）。
  void mouseDown(int col, int row) { selecting_ = true; selMoved_ = false; selR0_ = selR1_ = row; selC0_ = selC1_ = col; }
  void mouseDrag(int col, int row) { if (!selecting_) return; selR1_ = row; selC1_ = col; selMoved_ = true; }
  void mouseUp(int col, int row) {
    if (!selecting_) return; selecting_ = false;
    if (!selMoved_) { // ドラッグしていない＝クリック: 逆アセンブル行ならその命令へ
      if (mode_ == Mode::Normal && overlay_.empty() && !cmdMode_) { auto it = rowAddr_.find(row); if (it != rowAddr_.end()) cursor_ = it->second; }
      return;
    }
    selR1_ = row; selC1_ = col;
    std::string sel = term_.copyRegion(selR0_, selC0_, selR1_, selC1_);
    if (!sel.empty()) { if (FILE* pb = popen("/usr/bin/pbcopy", "w")) { fwrite(sel.data(), 1, sel.size(), pb); pclose(pb);
      status_ = tr_.tr(Msg::of(MsgId::UiCopied).num("n", (int64_t)sel.size())); } }
  }

  void key(int c) {
    if (mode_ == Mode::Picker) { pickerKey(c); return; }
    if (mode_ == Mode::BpEdit) { bpEditKey(c); return; }
    if (mode_ == Mode::Syms) { symsKey(c); return; }
    if (!overlay_.empty()) { overlayKey(c); return; }
    if (cmdMode_) { cmdKey(c); return; }
    switch (c) {
      case ':': cmdMode_ = true; cmdline_.clear(); status_.clear(); break;
      case 'q': quit_ = true; break;
      // TUI では継続・ステップは単一キー（c/s/n はコマンド表で BatchOnly）。エンジンへ直接 post する。
      case 's': if (dbg_.state() == RunState::Stopped) { cursor_ = 0; dbg_.post(cmd::StepInto{}); } break;
      case 'n': if (dbg_.state() == RunState::Stopped) { cursor_ = 0; dbg_.post(cmd::StepOver{}); } break;
      case 'c': if (dbg_.state() == RunState::Stopped) { cursor_ = 0; status_.clear(); dbg_.post(cmd::Continue{}); } break;
      case 'r': runLine("reg"); break;
      case 'l': layout_ = (layout_ + 1) % 4; status_ = tr_.tr(Msg::of(MsgId::UiLayoutChanged).num("n", layout_ + 1)); break;
      case 'b': { uint64_t a = cursor_ ? cursor_ : dbg_.stopInfo().pc; if (a) dbg_.post(cmd::ToggleBreakpointAt{a}); break; } // カーソル/PC にブレイク
      case '\t': { auto th = dbg_.threads(); if (th.size() > 1) { focus_ = (focus_ == Focus::Disasm) ? Focus::Threads : Focus::Disasm;
        if (focus_ == Focus::Threads) for (int i = 0; i < (int)th.size(); i++) // 選択を表示中スレッドへ合わせる
          if ((viewThread_ == 0 && th[i].isStopThread) || th[i].id == viewThread_) { threadSel_ = i; break; } }
        break; }
      case KeyEsc: if (dbg_.job().showing) { dbg_.cancel(); status_ = tr_.tr(Msg::of(MsgId::UiSearchCancelled)); } break;
      case KeyUp:
        if (focus_ == Focus::Threads) threadSelMove(-1);
        else if (cursor_) cursor_ = dbg_.insnBefore(viewTop(), cursor_);
        break;
      case KeyDown:
        if (focus_ == Focus::Threads) threadSelMove(+1);
        else if (cursor_) cursor_ = dbg_.insnAfter(cursor_);
        break;
      default: break;
    }
  }

  // ---- プロセスピッカ（:attach 無引数） ----
  std::vector<const ProcessInfo*> pickFiltered() {
    std::vector<const ProcessInfo*> v;
    for (const auto& p : pick_) {
      if (!pickFilter_.empty()) {
        std::string hay = std::to_string(p.pid) + " " + p.name + " " + p.path;
        if (hay.find(pickFilter_) == std::string::npos) continue;
      }
      v.push_back(&p);
    }
    return v;
  }
  void pickerKey(int c) {
    auto fl = pickFiltered();
    int n = (int)fl.size();
    int page = std::max(1, term_.rows() - 5);
    switch (c) {
      case KeyEsc: mode_ = Mode::Normal; status_.clear(); break;
      case '\r': case '\n':
        if (n > 0 && pickSel_ < n) { pid_t pid = fl[pickSel_]->pid; mode_ = Mode::Normal; pickFilter_.clear();
          dbg_.post(cmd::Attach{pid}); status_.clear(); }
        break;
      case KeyUp: case 16 /*C-p*/: if (pickSel_ > 0) pickSel_--; pickHScroll_ = 0; break;
      case KeyDown: case 14 /*C-n*/: if (pickSel_ < n - 1) pickSel_++; pickHScroll_ = 0; break;
      case 6 /*C-f*/: pickSel_ = std::min(n - 1, pickSel_ + page); pickHScroll_ = 0; break;
      case 2 /*C-b*/: pickSel_ = std::max(0, pickSel_ - page); pickHScroll_ = 0; break;
      case KeyRight: pickHScroll_ += 8; break;          // 引数を右へスクロール
      case KeyLeft:  pickHScroll_ = std::max(0, pickHScroll_ - 8); break;
      case 127: case 8: if (!pickFilter_.empty()) { pickFilter_.pop_back(); pickSel_ = 0; pickHScroll_ = 0; } break;
      default: if (c >= 0x20 && c < 0x7f) { pickFilter_ += (char)c; pickSel_ = 0; pickHScroll_ = 0; } break;
    }
  }

  // ---- ブレイク編集（:be） ----
  void reloadBpe() { bpe_ = dbg_.breakpoints(); if (bpeSel_ >= (int)bpe_.size()) bpeSel_ = std::max(0, (int)bpe_.size() - 1); }
  void bpEditKey(int c) {
    int n = (int)bpe_.size();
    switch (c) {
      case KeyEsc: case 'q': mode_ = Mode::Normal; status_.clear(); break;
      case KeyUp:   if (bpeSel_ > 0) bpeSel_--; break;
      case KeyDown: if (bpeSel_ < n - 1) bpeSel_++; break;
      case ' ': if (n > 0) { dbg_.post(cmd::ToggleEnabled{BpRef(bpe_[bpeSel_].id)}); } break;        // 有効⇄無効
      case 'd': case 127: if (n > 0) { dbg_.post(cmd::RemoveBreakpoint{BpRef(bpe_[bpeSel_].id)}); } break; // 削除
      case 'h': if (n > 0) { const auto& b = bpe_[bpeSel_]; // Soft⇄HW 実行 切替（ウォッチは対象外）
        if (b.kind == BpKind::Soft || b.kind == BpKind::HwExec) {
          BpKind nk = (b.kind == BpKind::Soft) ? BpKind::HwExec : BpKind::Soft;
          dbg_.post(cmd::RebuildBreakpoint{b.id, nk, b.addr, {}});
          status_ = tr_.tr(Msg::of(MsgId::UiBpEditKindToggled).num("id", b.id).str("kind", nk == BpKind::HwExec ? "HW" : "SW"));
        } } break;
      default: break;
    }
  }

  // ---- 対話シンボル一覧（:syms） ----
  static bool fuzzySubseq(const std::string& hay, const std::string& pat) {
    if (pat.empty()) return true;
    size_t j = 0;
    for (size_t i = 0; i < hay.size() && j < pat.size(); i++) if (hay[i] == pat[j]) j++;
    return j == pat.size();
  }
  void symsRebuild() {
    std::string lf; for (char c : symsFilter_) lf += (char)std::tolower((unsigned char)c);
    symsHits_.clear();
    for (const auto& e : symsAll_) {
      std::string ln; for (char c : e.name) ln += (char)std::tolower((unsigned char)c);
      // 空白区切りの各語が名前の部分列であれば一致（AND）
      bool ok = true; size_t i = 0, n = lf.size();
      while (i < n) { while (i < n && lf[i] == ' ') i++; size_t s = i; while (i < n && lf[i] != ' ') i++;
        if (s < i && !fuzzySubseq(ln, lf.substr(s, i - s))) { ok = false; break; } }
      if (ok) symsHits_.push_back(&e);
    }
    if (symsSortAddr_) std::sort(symsHits_.begin(), symsHits_.end(), [](const SymbolEntry* a, const SymbolEntry* b){ return a->addr < b->addr; });
    else std::sort(symsHits_.begin(), symsHits_.end(), [](const SymbolEntry* a, const SymbolEntry* b){ return a->name < b->name; });
    if (symsSel_ >= (int)symsHits_.size()) symsSel_ = std::max(0, (int)symsHits_.size() - 1);
  }
  void symsKey(int c) {
    int n = (int)symsHits_.size();
    switch (c) {
      case KeyEsc: mode_ = Mode::Normal; status_.clear(); break;
      case '\r': case '\n': if (n > 0 && symsSel_ < n) dbg_.post(cmd::ToggleBreakpointAt{symsHits_[symsSel_]->addr}); break;
      case '\t': symsSortAddr_ = !symsSortAddr_; symsRebuild(); break;
      case KeyUp:   if (symsSel_ > 0) symsSel_--; break;
      case KeyDown: if (symsSel_ < n - 1) symsSel_++; break;
      case 127: case 8: if (!symsFilter_.empty()) { symsFilter_.pop_back(); symsRebuild(); } break;
      default: if (c >= 0x20 && c < 0x7f) { symsFilter_ += (char)c; symsRebuild(); } break;
    }
  }
  void cmdKey(int c) {
    if (c == '\r' || c == '\n') { cmdMode_ = false; std::string l = cmdline_; cmdline_.clear(); runLine(l); }
    else if (c == KeyEsc) { cmdMode_ = false; cmdline_.clear(); }
    else if (c == 127 || c == 8) { if (!cmdline_.empty()) cmdline_.pop_back(); }
    else if (c == '\t') { auto comp = CommandInterpreter::complete(cmdline_, FrontEndKind::Tui); if (!comp.replacement.empty()) cmdline_ = comp.replacement; }
    else if (c >= 0x20 && c < 0x7f) cmdline_ += (char)c;
  }
  void overlayKey(int c) {
    int avail = term_.rows() - 2;
    int maxTop = (int)overlay_.size() - avail; if (maxTop < 0) maxTop = 0;
    switch (c) {
      case KeyEsc: case 'q': overlay_.clear(); overlayTop_ = 0; break;
      case KeyUp:   if (overlayTop_ > 0) overlayTop_--; break;
      case KeyDown: if (overlayTop_ < maxTop) overlayTop_++; break;
      case ' ': overlayTop_ = std::min(maxTop, overlayTop_ + avail); break;
      case 'b': overlayTop_ = std::max(0, overlayTop_ - avail); break;
      case 'g': overlayTop_ = 0; break;
      case 'G': overlayTop_ = maxTop; break;
      case ':': overlay_.clear(); overlayTop_ = 0; cmdMode_ = true; cmdline_.clear(); break;
      default: break;
    }
  }

  // ---- コマンド ----
  void runLine(const std::string& line) {
    ParseResult pr = interp_.execute(line, FrontEndKind::Tui);
    switch (pr.kind) {
      case ParseResult::Empty: break;
      case ParseResult::Error: status_ = tr_.tr(pr.error); break;
      case ParseResult::Ui: handleUi(pr.action); break;
      case ParseResult::Posted:
        if (pr.action.kind == UiAction::ShowExamine) lastExSpec_ = parseExSpec(pr.line); // :x の書式を覚える
        if (pr.action.kind != UiAction::None) pending_[pr.request] = pr.action; // 完了後のフォローアップ
        status_.clear();
        break;
      default: break;
    }
  }

  void drainEvents() {
    std::vector<std::pair<RequestId, CommandResult>> fin;
    std::vector<Msg> notices; bool stopped = false;
    { std::lock_guard<std::mutex> lk(sink_.mu); fin.swap(sink_.finished); notices.swap(sink_.notices);
      stopped = sink_.justStopped; sink_.justStopped = false; }
    if (stopped) { cursor_ = 0; status_.clear(); viewThread_ = 0; threadSel_ = 0; focus_ = Focus::Disasm; } // 新しい停止は停止スレッドへ・PC 追従
    for (auto& m : notices) { std::string s = tr_.tr(m); if (!s.empty()) status_ = s; }
    for (auto& [req, res] : fin) {
      // 結果メッセージを状態行へ（＋アドレス記述）
      auto lines = report_.commandResult(res);
      if (!lines.empty()) status_ = lines.front();
      auto it = pending_.find(req);
      if (it != pending_.end()) { followup(it->second, res); pending_.erase(it); }
    }
  }

  void followup(const UiAction& a, const CommandResult& res) {
    switch (a.kind) {
      case UiAction::ShowValue:
        if (res.value) { char b[96]; uint64_t v = *res.value;
          if (a.format == 'x') snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
          else if (a.format == 'd') snprintf(b, sizeof(b), "%lld", (long long)v);
          else snprintf(b, sizeof(b), "0x%llx (%lld)", (unsigned long long)v, (long long)v);
          status_ = a.text + " = " + b; }
        break;
      case UiAction::ShowExamine:
        if (auto ex = std::get_if<ExamineResult>(&res.payload)) {
          lastExAddr_ = ex->nextAddr; haveLastEx_ = true; // :x の継続位置を覚える
          openOverlay("x", report_.examine(*ex));
        }
        break;
      case UiAction::GotoDisasm:
        if (res.value) cursor_ = *res.value;
        break;
      case UiAction::ShowSymbol: {
        std::string w = (res.where && res.where->hasImage) ? formatDescribe(*res.where) : std::string();
        status_ = w.empty() ? tr_.tr(Msg::of(MsgId::UiSymNoneAt)) : w;
        break;
      }
      default: break;
    }
  }

  void handleUi(const UiAction& a) {
    switch (a.kind) {
      case UiAction::Quit: quit_ = true; break;
      case UiAction::ShowRegisters: openOverlay(tr_.tr(Msg::of(MsgId::UiRegsTitle)), report_.registers(dbg_.registers())); break;
      case UiAction::ShowRegister: { uint64_t v = 0; if (dbg_.readRegister(a.text, v)) { char b[32]; snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); status_ = a.text + " = " + b; } else status_ = tr_.tr(Msg::of(MsgId::UiUnknownRegister).str("name", a.text)); break; }
      case UiAction::ShowVectorRegisters: openOverlay(tr_.tr(Msg::of(MsgId::UiVecTitle)), report_.vectorRegisters(dbg_.vectorRegisters(), a.text)); break;
      case UiAction::ShowDebugRegisters: openOverlay(tr_.tr(Msg::of(MsgId::UiDrTitle)), report_.debugRegisters(dbg_.debugRegisters())); break;
      case UiAction::ShowSyscallWatch: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleSyscall)), report_.syscallWatch(dbg_.syscallWatch())); break;
      case UiAction::ShowSyscallStop: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleSyscall)), report_.syscallStop(dbg_.syscallWatch())); break;
      case UiAction::ShowSyscallNames: { SyscallNameList ns; Msg err;
        if (Debugger::syscallNames(a.text, ns, err)) openOverlay(tr_.tr(Msg::of(MsgId::UiTitleSyscallNames)), report_.syscallNames(ns));
        else status_ = tr_.tr(err); break; }
      case UiAction::SetBaseRelative: baseRel_ = a.flag; status_ = tr_.tr(Msg::of(a.flag ? MsgId::UiBaseoffOn : MsgId::UiBaseoffOff)); break;
      case UiAction::ShowBreakpoints: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleBreakpoints)), report_.breakpoints(dbg_.breakpoints())); break;
      case UiAction::ShowImages: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleImages)), report_.images(dbg_.images())); break;
      case UiAction::ShowSegments: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleSegments)), report_.segments(dbg_.segments(a.text))); break;
      case UiAction::ShowSymbols: { // 対話シンボル一覧（絞込・並び替え・Enter でブレイク）
        SymbolPage sp = dbg_.symbols(a.text, "", 0); // 全件取得（絞込は TUI 側で）
        symsAll_ = sp.entries; symsImage_ = sp.image; symsFilter_ = a.text2; symsSortAddr_ = true; symsSel_ = symsTop_ = 0;
        symsRebuild(); mode_ = Mode::Syms; break;
      }
      case UiAction::ShowAnnotations: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleAnnotations)), report_.annotations(dbg_.annotations(a.text))); break;
      case UiAction::ShowHelp: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleHelp)), report_.help()); break;
      case UiAction::ShowDefaultLib: { std::string d = dbg_.defaultLib(); status_ = d.empty() ? tr_.tr(Msg::of(MsgId::UiDefaultLibAutoShort)) : tr_.tr(Msg::of(MsgId::UiDefaultLib).str("name", d)); break; }
      case UiAction::FollowPc: cursor_ = 0; break; // PC へ追従
      case UiAction::OpenProcessPicker:
        pick_ = Debugger::processes(); pickFilter_.clear(); pickSel_ = pickTop_ = 0; mode_ = Mode::Picker; break;
      case UiAction::OpenBreakpointEditor:
        reloadBpe(); bpeSel_ = bpeTop_ = 0; mode_ = Mode::BpEdit; break;
      case UiAction::SetMouse:
        term_.setMouse(a.flag); status_ = tr_.tr(Msg::of(a.flag ? MsgId::UiMouseOn : MsgId::UiMouseOff)); break;
      case UiAction::ToggleMouse:
        term_.setMouse(!term_.mouseOn()); status_ = tr_.tr(Msg::of(term_.mouseOn() ? MsgId::UiMouseOn : MsgId::UiMouseOff)); break;
      case UiAction::ContinueExamine: continueExamine(a.text); break;
      default: status_ = tr_.tr(Msg::of(MsgId::UiCmdUnsupported)); break;
    }
  }

  void openOverlay(const std::string& title, std::vector<std::string> lines) {
    overlay_.clear(); overlay_.push_back(title); for (auto& l : lines) overlay_.push_back(l); overlayTop_ = 0;
  }

  // "x/8xb 式" / "x /8xb 式" から書式 "8xb" を取り出す（hExamine と同じ規則）。無ければ空。
  static std::string parseExSpec(const std::string& line) {
    size_t i = line.find_first_not_of(" \t"); if (i == std::string::npos) return "";
    size_t j = line.find_first_of(" \t", i); std::string cmd = line.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (cmd.size() > 2 && cmd[0] == 'x' && cmd[1] == '/') return cmd.substr(2);
    if (j == std::string::npos) return "";
    size_t k = line.find_first_not_of(" \t", j); if (k == std::string::npos) return "";
    size_t l = line.find_first_of(" \t", k); std::string t = line.substr(k, l == std::string::npos ? std::string::npos : l - k);
    return (!t.empty() && t[0] == '/') ? t.substr(1) : std::string();
  }

  // :x（式なし）の続き。前回位置から書式（override 優先）で再表示し、継続位置を進める。
  void continueExamine(const std::string& override) {
    if (!haveLastEx_) { status_ = tr_.tr(Msg::of(MsgId::UiExamineNoPrev)); return; }
    std::string spec = override.empty() ? lastExSpec_ : override;
    ExamineResult ex; Msg err;
    bool ok = spec.empty() ? dbg_.classicDump(lastExAddr_, 64, ex, err) : dbg_.examine(spec, lastExAddr_, ex, err);
    if (!ok) { status_ = tr_.tr(err); return; }
    lastExAddr_ = ex.nextAddr; lastExSpec_ = spec; haveLastEx_ = true;
    openOverlay("x", report_.examine(ex));
  }

  // 表示中スレッドの PC。停止スレッドは stopInfo、別スレッドは rip/pc レジスタから。
  uint64_t threadPc(ThreadId t) {
    if (t == 0) return dbg_.stopInfo().pc;
    auto rs = dbg_.registers(t);
    for (const auto& r : rs.regs) if (r.name == "rip" || r.name == "pc") return r.value;
    return 0;
  }
  // 表示スレッドの PC（停止中）。cursor_=0 は「PC 追従」。
  uint64_t viewTop() { return cursor_ ? cursor_ : threadPc(viewThread_); }

  // スレッド選択を dir 方向へ動かし、表示中スレッドを更新（逆アセ/reg/bt が追従）。
  void threadSelMove(int dir) {
    auto th = dbg_.threads(); int n = (int)th.size(); if (n == 0) return;
    threadSel_ = std::max(0, std::min(n - 1, threadSel_ + dir));
    viewThread_ = th[threadSel_].isStopThread ? 0 : th[threadSel_].id;
    cursor_ = 0; // 新スレッドは PC 追従
  }

  // ---- 描画 ----
  void render() {
    int rows = term_.rows(), cols = term_.cols();
    if (mode_ == Mode::Picker) { renderPicker(rows, cols); return; }
    if (mode_ == Mode::BpEdit) { renderBpEdit(rows, cols); return; }
    if (mode_ == Mode::Syms) { renderSyms(rows, cols); return; }
    if (!overlay_.empty()) { renderOverlay(rows, cols); return; }
    // ヘッダ
    std::string state = dbg_.state() == RunState::Stopped ? "stopped" : dbg_.state() == RunState::Running ? "running" : "idle";
    char hb[96]; snprintf(hb, sizeof(hb), "ta2tui  pid:%d  [%s]", (int)dbg_.pid(), state.c_str());
    std::string hdr = hb;
    if (dbg_.state() == RunState::Stopped) {
      auto si = dbg_.stopInfo();
      std::string w = formatDescribe(si.where);
      if (w.empty()) { char ab[24]; snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)si.pc); w = ab; }
      hdr += "  @ " + w;
    }
    term_.putAt(0, 0, "\x1b[7m" + ui::fitWidth(hdr, cols) + "\x1b[0m");

    if (dbg_.state() != RunState::Stopped) {
      for (int y = 1; y < rows - 1; y++) term_.clearLine(y); // 全画面モードからの残骸を消す
      term_.putAt(0, 2, dbg_.state() == RunState::Running ? tr_.tr(Msg::of(MsgId::UiRunningHint))
                                                          : tr_.tr(Msg::of(MsgId::UiIdleHint)));
      renderBottom(rows, cols);
      term_.flush();
      return;
    }

    // レイアウトで領域配分（0=標準 / 1=コード広め / 2=逆アセンブルのみ / 3=レジスタ重視）。
    int R = 6, B = 5;
    switch (layout_) {
      case 1: R = 4; B = 3; break;
      case 2: R = 0; B = 0; break;
      case 3: R = 12; B = 3; break;
      default: break;
    }
    // マルチスレッドのときだけスレッド枠 T（見出し＋最大 4 行）を確保。
    auto th = dbg_.threads();
    int T = (th.size() > 1 && layout_ != 2) ? std::min(5, 1 + (int)th.size()) : 0;
    int D = rows - 1 - R - B - T - 1; if (D < 3) { D = std::max(3, rows - 2); R = B = T = 0; }
    int row = 1;
    row = renderDisasm(row, D, cols);
    if (R > 0) row = renderRegs(row, R, cols);
    if (T > 0) row = renderThreads(row, T, cols, th);
    if (B > 0) row = renderBacktrace(row, B, cols);
    renderBottom(rows, cols);
    term_.flush();
  }

  // スレッド枠（★=停止スレッド / 👁=表示中。Tab でフォーカスし ↑↓ で表示切替）。
  int renderThreads(int top, int h, int cols, const std::vector<ThreadInfo>& th) {
    bool focus = (focus_ == Focus::Threads);
    ThreadId viewT = viewThread_;
    std::string title = std::string(focus ? "▶" : "") + "threads " + std::to_string(th.size());
    term_.putAt(0, top, "\x1b[2m" + title + "\x1b[0m");
    int y = 1;
    for (int i = 0; i < (int)th.size() && y < h; i++, y++) {
      const auto& t = th[i];
      bool isView = (viewT == 0 && t.isStopThread) || (viewT != 0 && t.id == viewT);
      char tb[64]; snprintf(tb, sizeof(tb), "%s%s tid 0x%x", t.isStopThread ? "★" : "  ", isView ? "👁" : "  ", (unsigned)t.id);
      std::string s = tb;
      if (focus && i == threadSel_) s = "\x1b[7m" + ui::fitWidth(s, cols) + "\x1b[0m";
      else s = ui::fitWidth(s, cols);
      term_.putAt(0, top + y, s);
    }
    for (; y < h; y++) term_.clearLine(top + y);
    return top + h;
  }

  int renderDisasm(int top, int h, int cols) {
    // 関数見出し・ソース行も 1 行と数えて枠の高さ h で打ち切る（withSource + maxRows）。
    DisasmOptions o; o.withSource = true; o.maxRows = h;
    DisasmResult dr = dbg_.disassemble(viewTop(), h, o, viewThread_);
    rowAddr_.clear();
    int y = 0;
    std::string lastSrc; // 直前に出したソース行（重複抑制）
    for (const auto& l : dr.lines) {
      if (y >= h) break;
      if (l.funcHeader && !l.funcHeader->empty()) { // 関数見出し（シアン）
        term_.putAt(0, top + y, ui::fitWidth("\x1b[36m" + *l.funcHeader + ":\x1b[0m", cols)); if (++y >= h) break;
      }
      if (l.source) { // ソース行（薄色）。同じ行の連続は出さない
        char sb[32]; snprintf(sb, sizeof(sb), "%s:%d", l.source->file.c_str(), l.source->line);
        std::string key = sb; std::string txt = l.source->text;
        size_t a0 = txt.find_first_not_of(" \t"); txt = (a0 == std::string::npos) ? std::string() : txt.substr(a0);
        std::string srcline = std::string("    \x1b[2m") + sb + "  " + txt + "\x1b[0m";
        if (key != lastSrc) { term_.putAt(0, top + y, ui::fitWidth(srcline, cols)); lastSrc = key; if (++y >= h) break; }
      }
      std::string mark = l.bp.kind != BpMark::None ? (l.bp.kind == BpMark::Soft ? "\x1b[31m●\x1b[0m" : "\x1b[33m◆\x1b[0m") : " ";
      std::string cur = l.isPc ? "\x1b[1m▸\x1b[0m" : " ";
      uint64_t disp = (baseRel_ && dr.imageBase) ? (l.addr - dr.imageBase) : l.addr;
      char ab[24]; snprintf(ab, sizeof(ab), "0x%012llx", (unsigned long long)disp);
      std::string line = mark + cur + " " + ab + "  " + l.mnemonic + (l.opStr.empty() ? "" : (" " + l.opStr));
      term_.putAt(0, top + y, ui::fitWidth(line, cols));
      rowAddr_[top + y] = l.addr; // この画面行＝この命令（マウスクリック用）
      y++;
    }
    for (; y < h; y++) term_.clearLine(top + y);
    return top + h;
  }

  // プロセスピッカ（:attach 無引数）: 一覧から選んで Enter でアタッチ。
  void renderPicker(int rows, int cols) {
    term_.putAt(0, 0, "\x1b[7m" + ui::fitWidth(tr_.tr(Msg::of(MsgId::UiPickerTitle)), cols) + "\x1b[0m");
    term_.putAt(0, 1, ui::fitWidth(tr_.tr(Msg::of(MsgId::UiPickerFilter)) + pickFilter_ + "\x1b[7m \x1b[0m", cols));
    term_.clearLine(2); // フィルタ行と一覧の間（残骸消し）
    auto fl = pickFiltered();
    int listTop = 3, avail = rows - listTop - 1;
    if (pickSel_ < pickTop_) pickTop_ = pickSel_;
    if (pickSel_ >= pickTop_ + avail) pickTop_ = pickSel_ - avail + 1;
    if (fl.empty()) term_.putAt(0, listTop, ui::fitWidth("  " + tr_.tr(Msg::of(MsgId::UiPickerEmpty)), cols));
    int y = 0;
    for (int i = pickTop_; i < (int)fl.size() && y < avail; i++, y++) {
      const ProcessInfo* p = fl[i];
      char pb[64]; snprintf(pb, sizeof(pb), "%6d  ", (int)p->pid);
      std::string na = p->attachable ? "" : ("  \x1b[2m" + tr_.tr(Msg::of(MsgId::UiPickerNotAttachable)) + "\x1b[0m");
      std::string body = std::string(pb) + p->name + (p->args.empty() ? "" : (" " + p->args)) + na;
      bool sel = (i == pickSel_);
      if (sel && pickHScroll_ > 0 && pickHScroll_ < (int)body.size()) body = "…" + body.substr(pickHScroll_); // 選択行だけ横スクロール
      term_.putAt(0, listTop + y, (sel ? "\x1b[7m" : "") + ui::fitWidth((sel ? "▸ " : "  ") + body, cols) + (sel ? "\x1b[0m" : ""));
    }
    for (; y < avail; y++) term_.clearLine(listTop + y);
    term_.putAt(0, rows - 1, "\x1b[2m" + ui::fitWidth(tr_.tr(Msg::of(MsgId::UiPickerHint)), cols) + "\x1b[0m");
    term_.flush();
  }

  // ブレイク編集（:be）: 選んで Space 有効⇄無効 / d 削除。
  void renderBpEdit(int rows, int cols) {
    reloadBpe(); // 操作が即反映されるよう毎フレーム引き直す
    term_.putAt(0, 0, "\x1b[7m" + ui::fitWidth(tr_.tr(Msg::of(MsgId::UiBpEditTitle)), cols) + "\x1b[0m");
    term_.clearLine(1); // 見出しと一覧の間（残骸消し）
    int listTop = 2, avail = rows - listTop - 1;
    if (bpeSel_ < bpeTop_) bpeTop_ = bpeSel_;
    if (bpeSel_ >= bpeTop_ + avail) bpeTop_ = bpeSel_ - avail + 1;
    auto lines = report_.breakpoints(bpe_); // 1 行目は見出し。本体は #id ...
    if (bpe_.empty()) term_.putAt(0, listTop, ui::fitWidth("  " + tr_.tr(Msg::of(MsgId::UiBpEditEmpty)), cols));
    int y = 0;
    for (int i = bpeTop_; i < (int)bpe_.size() && y < avail; i++, y++) {
      // breakpoints() の本体行は見出し 1 行の後に続く（lines[1..]）。
      std::string body = (i + 1 < (int)lines.size()) ? lines[i + 1] : std::string();
      bool sel = (i == bpeSel_);
      term_.putAt(0, listTop + y, (sel ? "\x1b[7m" : "") + ui::fitWidth((sel ? "▸ " : "  ") + body, cols) + (sel ? "\x1b[0m" : ""));
    }
    for (; y < avail; y++) term_.clearLine(listTop + y);
    term_.putAt(0, rows - 1, "\x1b[2m" + ui::fitWidth(tr_.tr(Msg::of(MsgId::UiBpEditHint)), cols) + "\x1b[0m");
    term_.flush();
  }

  // 対話シンボル一覧（:syms）: 絞込・並び替え・Enter でブレイク設定/解除。
  void renderSyms(int rows, int cols) {
    std::string sort = tr_.tr(Msg::of(symsSortAddr_ ? MsgId::UiSymsSortAddr : MsgId::UiSymsSortName));
    std::string title = tr_.tr(Msg::of(MsgId::UiSymsTitle).str("image", symsImage_).num("hits", (int64_t)symsHits_.size())
                                 .num("total", (int64_t)symsAll_.size()).str("sort", sort));
    term_.putAt(0, 0, "\x1b[7m" + ui::fitWidth(title, cols) + "\x1b[0m");
    term_.putAt(0, 1, ui::fitWidth(tr_.tr(Msg::of(MsgId::UiSymsFilter)) + symsFilter_ + "\x1b[7m \x1b[0m", cols));
    term_.clearLine(2);
    // いまブレイクがあるアドレス集合（● 印用）
    std::unordered_map<uint64_t, bool> bpAddr; for (const auto& b : dbg_.breakpoints()) bpAddr[b.addr] = true;
    int listTop = 3, avail = rows - listTop - 1;
    if (symsSel_ < symsTop_) symsTop_ = symsSel_;
    if (symsSel_ >= symsTop_ + avail) symsTop_ = symsSel_ - avail + 1;
    if (symsHits_.empty()) term_.putAt(0, listTop, ui::fitWidth("  " + tr_.tr(Msg::of(MsgId::UiSymsEmpty)), cols));
    int y = 0;
    for (int i = symsTop_; i < (int)symsHits_.size() && y < avail; i++, y++) {
      const SymbolEntry* e = symsHits_[i];
      std::string mark = bpAddr.count(e->addr) ? "\x1b[31m●\x1b[0m" : " ";
      char ab[24]; snprintf(ab, sizeof(ab), "0x%012llx", (unsigned long long)e->addr);
      std::string body = mark + " " + ab + "  " + e->name;
      bool sel = (i == symsSel_);
      term_.putAt(0, listTop + y, (sel ? "\x1b[7m" : "") + ui::fitWidth((sel ? "▸" : " ") + body, cols) + (sel ? "\x1b[0m" : ""));
    }
    for (; y < avail; y++) term_.clearLine(listTop + y);
    term_.putAt(0, rows - 1, "\x1b[2m" + ui::fitWidth(tr_.tr(Msg::of(MsgId::UiSymsHint)), cols) + "\x1b[0m");
    term_.flush();
  }

  int renderRegs(int top, int h, int cols) {
    auto rs = dbg_.registers(viewThread_);
    std::vector<std::string> cells;
    for (auto& r : rs.regs) { char b[48]; snprintf(b, sizeof(b), "%-6s=0x%016llx", r.name.c_str(), (unsigned long long)r.value); cells.push_back(b); }
    int colW = 26, ncol = std::max(1, cols / colW), y = 0;
    for (size_t i = 0; i < cells.size() && y < h; ) {
      std::string line;
      for (int c = 0; c < ncol && i < cells.size(); c++, i++) line += ui::fitWidth(cells[i], colW) ;
      term_.putAt(0, top + y, ui::fitWidth(line, cols)); y++;
    }
    for (; y < h; y++) term_.clearLine(top + y);
    return top + h;
  }

  int renderBacktrace(int top, int h, int cols) {
    term_.putAt(0, top, "\x1b[2mbacktrace\x1b[0m");
    auto frames = dbg_.backtrace(viewThread_, viewThread_ != 0); // 別スレッドは軽量走査
    int y = 1;
    for (auto& f : frames) { if (y >= h) break; term_.putAt(0, top + y, ui::fitWidth("  " + report_.frame(f), cols)); y++; }
    for (; y < h; y++) term_.clearLine(top + y);
    return top + h;
  }

  void renderBottom(int rows, int cols) {
    std::string line;
    JobStatus js = dbg_.job();
    if (js.showing) // 重い全イメージ探索中は進捗＋ESC 中止を出す（UI は固まらない）
      line = "\x1b[33m" + tr_.tr(Msg::of(MsgId::UiSearchProgress).num("done", js.done).num("total", js.total)) + "\x1b[0m";
    else line = cmdMode_ ? (":" + cmdline_ + "\x1b[7m \x1b[0m") : status_;
    term_.putAt(0, rows - 1, ui::fitWidth(line, cols));
  }

  void renderOverlay(int rows, int cols) {
    std::string title = overlay_.empty() ? "" : overlay_[0];
    term_.putAt(0, 0, "\x1b[33m" + ui::fitWidth(title + "   \x1b[2m" + tr_.tr(Msg::of(MsgId::UiPagerHint)), cols) + "\x1b[0m");
    int avail = rows - 2, y = 0;
    for (; y < avail; y++) {
      int idx = 1 + overlayTop_ + y;
      term_.putAt(0, 1 + y, idx < (int)overlay_.size() ? ui::fitWidth(overlay_[idx], cols) : std::string(""));
    }
    term_.clearLine(rows - 1);
    term_.flush();
  }

  Debugger dbg_;
  Translator tr_;
  CommandInterpreter interp_;
  TuiSink sink_;
  ui::TextReport report_;
  ui::Terminal term_;
  SubscriptionId sub_ = 0;

  enum class Mode { Normal, Picker, BpEdit, Syms }; // 全画面の対話モード
  Mode mode_ = Mode::Normal;
  enum class Focus { Disasm, Threads };       // ↑↓ の対象（Tab で切替）
  Focus focus_ = Focus::Disasm;
  ThreadId viewThread_ = 0;  // 表示中スレッド（0=停止スレッド）。他スレッドの reg/逆アセ/bt を覗く
  int threadSel_ = 0;        // スレッド一覧の選択位置

  bool quit_ = false, cmdMode_ = false;
  std::string cmdline_, status_;
  std::vector<std::string> overlay_; int overlayTop_ = 0;
  uint64_t cursor_ = 0; // 0 = PC 追従
  bool baseRel_ = false; // 逆アセンブルのアドレスをイメージ先頭相対で表示（:baseoff）
  int layout_ = 0;       // 0=標準 / 1=コード広め / 2=逆アセンブルのみ / 3=レジスタ重視（l で巡回）
  std::unordered_map<RequestId, UiAction> pending_;

  // :x（式なし）の継続位置。継続を持つのは L3（CommandInterpreter.cpp hExamine 参照）。
  std::string lastExSpec_; uint64_t lastExAddr_ = 0; bool haveLastEx_ = false;
  // プロセスピッカ（:attach 無引数）
  std::vector<ProcessInfo> pick_; std::string pickFilter_; int pickSel_ = 0, pickTop_ = 0, pickHScroll_ = 0;
  // ブレイク編集（:be）
  std::vector<BreakpointInfo> bpe_; int bpeSel_ = 0, bpeTop_ = 0;
  // 対話シンボル一覧（:syms）
  std::vector<SymbolEntry> symsAll_; std::vector<const SymbolEntry*> symsHits_;
  std::string symsImage_, symsFilter_; bool symsSortAddr_ = true; int symsSel_ = 0, symsTop_ = 0;
  // マウス: 逆アセンブル画面の各行→命令アドレス（クリックでカーソル移動）
  std::unordered_map<int, uint64_t> rowAddr_;
  bool selecting_ = false, selMoved_ = false; int selR0_ = 0, selC0_ = 0, selR1_ = 0, selC1_ = 0; // ドラッグ選択
};

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> target;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { fputs("ta2tui [pid | パス [引数...]]   対話 TUI（自作プログラム専用。要 root）\n", stdout); return 0; }
    target.push_back(a);
  }
  if (geteuid() != 0) { fprintf(stderr, "root 権限で実行してください\n"); return 1; }

  if (pipe(g_wakePipe) == 0) { fcntl(g_wakePipe[0], F_SETFL, O_NONBLOCK); fcntl(g_wakePipe[1], F_SETFL, O_NONBLOCK); }
  struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = onWinch; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &sa, nullptr);

  std::vector<std::string> initial;
  if (!target.empty()) {
    bool isPid = !target[0].empty(); for (char ch : target[0]) if (ch < '0' || ch > '9') isPid = false;
    if (isPid) initial.push_back("attach " + target[0]);
    else { std::string r = "run"; for (auto& t : target) r += " " + t; initial.push_back(r); }
  }

  TuiApp app;
  return app.run(initial);
}
