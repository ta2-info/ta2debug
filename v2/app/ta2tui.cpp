// L3 Presentation: ta2tui — 対話 TUI 正面。v1 の TUI を v2 ヘッドレス Debugger の上へ写した第一カット。
// エンジンは threaded=true（専用スレッドが受信箱を処理）。イベントは IEventSink で来る（エンジンスレッド）
// ので self-pipe でメインスレッドを起こし、メインは Debugger のクエリを引き直して再描画する。入力は
// 同じ poll で拾う。描画は Terminal（代替画面・raw）＋ TextReport。コマンド解釈は CommandInterpreter(Tui)。
#include <algorithm>
#include <atomic>
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
  TuiApp() : dbg_(EngineOptions::interactive()), tr_(Catalog(), detectLocaleLang()),
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
      }
      if (in[i] == 0x1b) { key(KeyEsc); i++; continue; }
      key((unsigned char)in[i]); i++;
    }
  }

  enum { KeyUp = 0x1000, KeyDown, KeyLeft, KeyRight, KeyEsc = 0x1b };

  void key(int c) {
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
      case KeyUp:   if (cursor_) cursor_ = dbg_.insnBefore(viewTop(), cursor_); break;
      case KeyDown: if (cursor_) cursor_ = dbg_.insnAfter(cursor_); break;
      default: break;
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
    if (stopped) { cursor_ = 0; status_.clear(); } // 新しい停止は PC 追従・前の結果行を消す
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
        if (auto ex = std::get_if<ExamineResult>(&res.payload)) openOverlay("x", report_.examine(*ex));
        break;
      case UiAction::GotoDisasm:
        if (res.value) cursor_ = *res.value;
        break;
      default: break;
    }
  }

  void handleUi(const UiAction& a) {
    switch (a.kind) {
      case UiAction::Quit: quit_ = true; break;
      case UiAction::ShowRegisters: openOverlay(tr_.tr(Msg::of(MsgId::UiRegsTitle)), report_.registers(dbg_.registers())); break;
      case UiAction::ShowRegister: { uint64_t v = 0; if (dbg_.readRegister(a.text, v)) { char b[32]; snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); status_ = a.text + " = " + b; } else status_ = tr_.tr(Msg::of(MsgId::UiUnknownRegister).str("name", a.text)); break; }
      case UiAction::ShowBreakpoints: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleBreakpoints)), report_.breakpoints(dbg_.breakpoints())); break;
      case UiAction::ShowImages: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleImages)), report_.images(dbg_.images())); break;
      case UiAction::ShowSegments: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleSegments)), report_.segments(dbg_.segments(a.text))); break;
      case UiAction::ShowSymbols: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleSymbols)), report_.symbols(dbg_.symbols(a.text, a.text2, 0))); break;
      case UiAction::ShowAnnotations: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleAnnotations)), report_.annotations(dbg_.annotations(a.text))); break;
      case UiAction::ShowHelp: openOverlay(tr_.tr(Msg::of(MsgId::UiTitleHelp)), report_.help()); break;
      case UiAction::ShowDefaultLib: { std::string d = dbg_.defaultLib(); status_ = d.empty() ? tr_.tr(Msg::of(MsgId::UiDefaultLibAutoShort)) : tr_.tr(Msg::of(MsgId::UiDefaultLib).str("name", d)); break; }
      case UiAction::FollowPc: cursor_ = 0; break; // PC へ追従
      default: status_ = tr_.tr(Msg::of(MsgId::UiCmdUnsupported)); break;
    }
  }

  void openOverlay(const std::string& title, std::vector<std::string> lines) {
    overlay_.clear(); overlay_.push_back(title); for (auto& l : lines) overlay_.push_back(l); overlayTop_ = 0;
  }

  // 表示スレッドの PC（停止中）。cursor_=0 は「PC 追従」。
  uint64_t viewTop() { uint64_t pc = 0; auto si = dbg_.stopInfo(); pc = si.pc; return cursor_ ? cursor_ : pc; }

  // ---- 描画 ----
  void render() {
    int rows = term_.rows(), cols = term_.cols();
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
      term_.putAt(0, 2, dbg_.state() == RunState::Running ? tr_.tr(Msg::of(MsgId::UiRunningHint))
                                                          : tr_.tr(Msg::of(MsgId::UiIdleHint)));
      renderBottom(rows, cols);
      term_.flush();
      return;
    }

    // 領域配分: ヘッダ(1) + 逆アセンブル(D) + レジスタ(R) + バックトレース(B) + 状態(1)
    int R = 6, B = 5;
    int D = rows - 1 - R - B - 1; if (D < 3) { D = std::max(3, rows - 2); R = 0; B = 0; }
    int row = 1;
    row = renderDisasm(row, D, cols);
    if (R > 0) row = renderRegs(row, R, cols);
    if (B > 0) row = renderBacktrace(row, B, cols);
    renderBottom(rows, cols);
    term_.flush();
  }

  int renderDisasm(int top, int h, int cols) {
    DisasmResult dr = dbg_.disassemble(viewTop(), h);
    int y = 0;
    for (const auto& l : dr.lines) {
      if (y >= h) break;
      std::string mark = l.bp.kind != BpMark::None ? (l.bp.kind == BpMark::Soft ? "\x1b[31m●\x1b[0m" : "\x1b[33m◆\x1b[0m") : " ";
      std::string cur = l.isPc ? "\x1b[1m▸\x1b[0m" : " ";
      char ab[24]; snprintf(ab, sizeof(ab), "0x%012llx", (unsigned long long)l.addr);
      std::string line = mark + cur + " " + ab + "  " + l.mnemonic + (l.opStr.empty() ? "" : (" " + l.opStr));
      term_.putAt(0, top + y, ui::fitWidth(line, cols));
      y++;
    }
    for (; y < h; y++) term_.clearLine(top + y);
    return top + h;
  }

  int renderRegs(int top, int h, int cols) {
    auto rs = dbg_.registers();
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
    auto frames = dbg_.backtrace();
    int y = 1;
    for (auto& f : frames) { if (y >= h) break; term_.putAt(0, top + y, ui::fitWidth("  " + report_.frame(f), cols)); y++; }
    for (; y < h; y++) term_.clearLine(top + y);
    return top + h;
  }

  void renderBottom(int rows, int cols) {
    std::string line = cmdMode_ ? (":" + cmdline_ + "\x1b[7m \x1b[0m") : status_;
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

  bool quit_ = false, cmdMode_ = false;
  std::string cmdline_, status_;
  std::vector<std::string> overlay_; int overlayTop_ = 0;
  uint64_t cursor_ = 0; // 0 = PC 追従
  std::unordered_map<RequestId, UiAction> pending_;
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
