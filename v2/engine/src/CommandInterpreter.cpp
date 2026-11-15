// コマンド表と 1 行の解釈。v1 main.cpp runCommand の各分岐（引数の切り方・既定値・判定順）と
// batchExec のバッチ専用コマンドを、単一の表（CommandSpec）＋コマンドごとのハンドラへ移したもの。
// ハンドラは「構文と事前条件の確認 → Command を post / UiAction を返す」だけを行い、対象には触らない。
#include "engine/CommandInterpreter.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "core/MemoryExaminer.h"

namespace ta2 {
namespace engine {

struct CommandContext {
  Debugger& dbg;
  const CommandSpec& spec;
  std::string cmd;   // 入力された綴り（別名・"x/4xg" の形のまま）
  std::string arg;   // 最初の空白より後ろ（v1 と同じく切り詰めない）
  FrontEndKind fe;
};

namespace {

using Ctx = const CommandContext&;

uint64_t parseNum(const std::string& s) { return strtoull(s.c_str(), nullptr, 0); }

// 空白区切りの語に分ける（連続する空白は 1 つ扱い）。
std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> toks; std::string cur;
  for (char ch : s) {
    if (ch == ' ') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
    else cur.push_back(ch);
  }
  if (!cur.empty()) toks.push_back(cur);
  return toks;
}

// "a,b,c" を最大 maxParts 個に分割（式にカンマは無い前提）。余り（パス等）は最後の要素に残す。
std::vector<std::string> splitComma(const std::string& s, int maxParts) {
  std::vector<std::string> out; size_t i = 0;
  while ((int)out.size() < maxParts - 1) {
    size_t c = s.find(',', i);
    if (c == std::string::npos) break;
    out.push_back(s.substr(i, c - i)); i = c + 1;
  }
  out.push_back(s.substr(i));
  return out;
}

std::string trimStr(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t"); return s.substr(a, b - a + 1);
}

ParseResult errorResult(Msg m) {
  ParseResult r;
  r.kind = ParseResult::Error;
  r.error = std::move(m);
  return r;
}
ParseResult errorResult(MsgId id) { return errorResult(Msg::of(id)); }
ParseResult usage(Ctx c) { return errorResult(c.spec.usage); }

UiAction action(UiAction::Kind k, std::string text = std::string(), std::string text2 = std::string()) {
  UiAction a;
  a.kind = k; a.text = std::move(text); a.text2 = std::move(text2);
  return a;
}
ParseResult ui(UiAction a) {
  ParseResult r;
  r.kind = ParseResult::Ui;
  r.action = std::move(a);
  return r;
}
ParseResult ui(UiAction::Kind k) { return ui(action(k)); }

ParseResult posted(Ctx c, Command cmd, UiAction follow = UiAction()) {
  ParseResult r;
  r.kind = ParseResult::Posted;
  r.action = std::move(follow);
  r.request = c.dbg.post(std::move(cmd));
  return r;
}
// 対象を走らせるコマンド: バッチは完了後に停止を待つ（v1 batchExec の willStop）。
UiAction afterResume(Ctx c) {
  return c.fe == FrontEndKind::Batch ? action(UiAction::WaitForStop) : UiAction();
}

// ---- 対象 ----

ParseResult hAttach(Ctx c) {
  if (c.dbg.attached()) return errorResult(MsgId::ErrAlreadyAttached);
  if (c.arg.empty()) return ui(UiAction::OpenProcessPicker); // pid 無し → 一覧から選択
  cmd::Attach a;
  a.pid = (pid_t)parseNum(c.arg);
  return posted(c, a);
}

ParseResult hRun(Ctx c) {
  if (c.dbg.attached()) return errorResult(MsgId::ErrAlreadyLaunched);
  if (c.arg.empty()) return usage(c);
  cmd::Launch l;
  l.args = splitWords(c.arg);
  return posted(c, l, afterResume(c));
}

ParseResult hRestart(Ctx c) { return posted(c, cmd::Restart{}); }
ParseResult hDetach(Ctx c) { return posted(c, cmd::Detach{}); }
ParseResult hQuit(Ctx) { return ui(UiAction::Quit); }

// ---- 実行 ----

ParseResult hAuto(Ctx c) {
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrAutoNeedsStop);
  auto toks = splitWords(c.arg);
  unsigned mask = 0;
  if (toks.empty()) mask = 1 | 2 | 4; // 既定: call/syscall/ret すべて
  else for (auto& t : toks) {
    if (t == "call") mask |= 1;
    else if (t == "syscall" || t == "sc") mask |= 2;
    else if (t == "ret") mask |= 4;
    else if (t == "all") mask |= 7;
    else return errorResult(MsgId::ErrAutoBadArg);
  }
  cmd::Auto a;
  a.mask = mask;
  return posted(c, a, afterResume(c));
}

ParseResult hContinue(Ctx c) {
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrNotStopped);
  return posted(c, cmd::Continue{}, afterResume(c));
}
ParseResult hStep(Ctx c) {
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrNotStopped);
  return posted(c, cmd::StepInto{}, afterResume(c));
}
ParseResult hNext(Ctx c) {
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrNotStopped);
  return posted(c, cmd::StepOver{}, afterResume(c));
}
ParseResult hWait(Ctx c) {
  if (c.dbg.state() != RunState::Running) return errorResult(MsgId::ErrNotRunning);
  return ui(UiAction::WaitForStop);
}
ParseResult hWhere(Ctx c) {
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrNotStopped);
  return ui(UiAction::PrintBacktrace);
}

// ---- ブレイク ----

ParseResult hBreak(Ctx c) {
  cmd::AddBreakpoint b;
  b.at = AddrSpec(c.arg);
  b.kind = BpKind::Soft;
  return posted(c, b);
}
ParseResult hHwBreak(Ctx c) {
  cmd::AddBreakpoint b;
  b.at = AddrSpec(c.arg);
  b.kind = BpKind::HwExec;
  return posted(c, b);
}
ParseResult hWatch(Ctx c) {
  // 空白区切り: 先頭=式、残り=サイズ/種別（順不同）
  auto toks = splitWords(c.arg);
  if (toks.empty()) return usage(c);
  int size = 8; bool onR = false, onW = false;
  for (size_t i = 1; i < toks.size(); i++) {
    const std::string& t = toks[i];
    if (t == "1" || t == "2" || t == "4" || t == "8") size = atoi(t.c_str());
    else if (t == "r" || t == "w" || t == "rw" || t == "wr") {
      onR = t.find('r') != std::string::npos; onW = t.find('w') != std::string::npos;
    }
    else return errorResult(Msg::of(MsgId::ErrWatchBadArg).str("arg", t));
  }
  if (!onR && !onW) onW = true; // 既定は書込監視
  cmd::AddBreakpoint b;
  b.at = AddrSpec(toks[0]);
  b.kind = BpKind::Watch;
  b.watch.size = size; b.watch.onRead = onR; b.watch.onWrite = onW;
  return posted(c, b);
}
ParseResult hBreakList(Ctx) { return ui(UiAction::ShowBreakpoints); }
ParseResult hBreakEdit(Ctx) { return ui(UiAction::OpenBreakpointEditor); }

// 引数を「#番号」または式として解釈する（v1 withBreakId）。
BpRef breakRef(const std::string& arg) {
  if (!arg.empty() && arg[0] == '#') return BpRef(atoi(arg.c_str() + 1));
  return BpRef(AddrSpec(arg));
}
ParseResult hBreakDelete(Ctx c) {
  if (c.arg.empty()) return usage(c);
  cmd::RemoveBreakpoint r;
  r.ref = breakRef(c.arg);
  return posted(c, r);
}
ParseResult hBreakToggle(Ctx c) {
  if (c.arg.empty()) return usage(c);
  cmd::ToggleEnabled t;
  t.ref = breakRef(c.arg);
  return posted(c, t);
}

// ---- 参照 ----

ParseResult hSym(Ctx c) {
  cmd::Describe d;
  d.at = AddrSpec(c.arg);
  return posted(c, d, action(UiAction::ShowSymbol));
}

ParseResult hDis(Ctx c) {
  if (c.fe == FrontEndKind::Batch) {
    // バッチにはペインが無いので、逆アセンブルをテキスト出力する（x/Ni 相当。v1 disToOutput）。
    auto toks = splitWords(c.arg);
    cmd::Examine e;
    int count = 20;
    if (toks.empty()) {
      if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::UsageDisBatch);
      e.at = AddrSpec(c.dbg.stopInfo().pc);
    } else {
      e.at = AddrSpec(toks[0]);
      if (toks.size() > 1) { int n = (int)parseNum(toks[1]); if (n > 0) count = n; }
    }
    e.spec = std::to_string(count) + "i";
    return posted(c, e, action(UiAction::ShowExamine));
  }
  // TUI: 逆アセンブル画面（ペイン）を移動する。dis <式> でそのアドレスから、dis で現在 PC へ。
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrDisNeedsStop);
  std::string a = trimStr(c.arg);
  if (a.empty()) return ui(UiAction::FollowPc);
  cmd::Evaluate e;
  e.expr = a;
  return posted(c, e, action(UiAction::GotoDisasm));
}

ParseResult hReg(Ctx c) {
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrRegNeedsStop);
  const std::string& arg = c.arg;
  size_t eq = arg.find('=');
  if (eq != std::string::npos) { // 書き込み  reg <名前>=<式>
    auto trim = [](std::string s) {
      size_t a = s.find_first_not_of(' '); if (a == std::string::npos) return std::string();
      size_t b = s.find_last_not_of(' '); return s.substr(a, b - a + 1);
    };
    std::string name = trim(arg.substr(0, eq)), vexpr = trim(arg.substr(eq + 1));
    if (name.empty() || vexpr.empty()) return usage(c);
    cmd::SetRegister s;
    s.name = name; s.value = AddrSpec(vexpr);
    return posted(c, s);
  }
  // ベクタ/浮動小数レジスタ（族名 + 番号、または族名 / vec / fp / fpregs で全部）。族名は Arch が決める。
  {
    std::string a = arg; if (!a.empty() && (a[0] == '$' || a[0] == '%')) a = a.substr(1);
    ArchInfo ai = c.dbg.arch();
    const std::string& fam = ai.vectorFamily; // "xmm" / "v"
    bool wantAll = (a == "vec" || a == "fp" || a == "fpregs" || (!fam.empty() && a == fam));
    bool wantOne = !fam.empty() && a.size() > fam.size() && a.rfind(fam, 0) == 0 &&
                   (fam.size() > 1 || std::isdigit((unsigned char)a[fam.size()]));
    if (wantAll) return ui(action(UiAction::ShowVectorRegisters));
    if (wantOne) return ui(action(UiAction::ShowVectorRegisters, a));
  }
  if (!arg.empty()) return ui(action(UiAction::ShowRegister, arg)); // 1 レジスタ参照
  return ui(UiAction::ShowRegisters);
}

ParseResult hDr(Ctx c) {
  if (c.dbg.state() != RunState::Stopped) return errorResult(MsgId::ErrDrNeedsStop);
  return ui(UiAction::ShowDebugRegisters);
}

ParseResult hPrint(Ctx c) {
  // 式の値を表示（p=10進+16進, p/x=16進, p/d=10進）。
  char fmt = (c.cmd.size() > 2) ? c.cmd[2] : 0;
  if (c.arg.empty()) return usage(c);
  cmd::Evaluate e;
  e.expr = c.arg;
  UiAction a = action(UiAction::ShowValue, trimStr(c.arg));
  a.format = fmt;
  return posted(c, e, a);
}

ParseResult hExamine(Ctx c) {
  // 書式指定は "x/4xg 式" でも "x /4xg 式" でも可。無指定なら従来の 16 進ダンプ。
  std::string spec; if (c.cmd.size() > 2) spec = c.cmd.substr(2); // "x/..." 形式
  auto toks = splitWords(c.arg);
  if (spec.empty() && !toks.empty() && !toks[0].empty() && toks[0][0] == '/') {
    spec = toks[0].substr(1); toks.erase(toks.begin());
  }
  // 式が無ければ「前回の続き」。書式だけ変えて続けることもできる（例: x/i → x）。
  // 前回が無いとき使用法を出すのは表示側（継続位置を持っているのは L3）。
  if (toks.empty()) return ui(action(UiAction::ContinueExamine, spec));
  cmd::Examine e;
  e.at = AddrSpec(toks[0]);
  e.spec = spec;
  e.classicBytes = 64;
  if (spec.empty() && toks.size() > 1) e.classicBytes = (int)parseNum(toks[1]);
  return posted(c, e, action(UiAction::ShowExamine));
}

ParseResult hImages(Ctx) { return ui(UiAction::ShowImages); }
ParseResult hSegments(Ctx c) { return ui(action(UiAction::ShowSegments, c.arg)); }

ParseResult hSymbols(Ctx c) {
  // 第1語=ライブラリ名（省略で既定ライブラリ）、残り=絞り込み（部分一致・大小無視）
  auto toks = splitWords(c.arg);
  std::string lib, filt;
  if (!toks.empty()) {
    lib = toks[0];
    for (size_t i = 1; i < toks.size(); i++) filt += (i > 1 ? " " : "") + toks[i];
  }
  return ui(action(UiAction::ShowSymbols, lib, filt));
}

ParseResult hUselib(Ctx c) {
  std::string a = trimStr(c.arg);
  if (a.empty()) return ui(UiAction::ShowDefaultLib);
  cmd::SetDefaultLib s;
  if (a == "-") return posted(c, s); // 解除
  if (!c.dbg.attached()) return errorResult(MsgId::ErrNeedAttach);
  s.name = a;
  return posted(c, s);
}

// ---- 書き換え ----

ParseResult hSetmemHex(Ctx c) {
  auto parts = splitComma(c.arg, 2);
  if (parts.size() < 2) return usage(c);
  cmd::WriteMemory w; // バイト列（HEX）をそのまま書く
  if (!parseHexBytes(parts[1], w.bytes)) return errorResult(MsgId::ErrMemHexBad);
  w.at = AddrSpec(trimStr(parts[0]));
  return posted(c, w);
}

ParseResult hSetmemN(Ctx c) {
  auto parts = splitComma(c.arg, 2);
  if (parts.size() < 2) return usage(c);
  cmd::WriteValue w; // N バイトの値を書く（リトルエンディアン）
  w.size = c.cmd.size() > 6 ? c.cmd[6] - '0' : 8;
  w.at = AddrSpec(trimStr(parts[0]));
  w.value = AddrSpec(trimStr(parts[1]));
  return posted(c, w);
}

ParseResult hMemset(Ctx c) {
  auto parts = splitComma(c.arg, 3);
  if (parts.size() < 3) return usage(c);
  cmd::FillMemory f;
  f.at = AddrSpec(trimStr(parts[0]));
  f.value = AddrSpec(trimStr(parts[1]));
  f.size = AddrSpec(trimStr(parts[2]));
  return posted(c, f);
}

ParseResult hFileIo(Ctx c) {
  auto parts = splitComma(c.arg, 3); // addr, size, path（path は残り全部）
  if (parts.size() < 3) return errorResult(Msg::of(MsgId::UsageFileIo).str("cmd", c.cmd));
  if (c.cmd == "wf") { // 対象メモリ → ファイル
    cmd::DumpToFile d;
    d.at = AddrSpec(trimStr(parts[0])); d.size = AddrSpec(trimStr(parts[1])); d.path = trimStr(parts[2]);
    return posted(c, d);
  }
  cmd::LoadFromFile l;  // ファイル → 対象メモリ
  l.at = AddrSpec(trimStr(parts[0])); l.size = AddrSpec(trimStr(parts[1])); l.path = trimStr(parts[2]);
  return posted(c, l);
}

// ---- 注釈 ----

ParseResult hAnn(Ctx c) {
  auto parts = splitComma(c.arg, 3); // <式>,<名前>[,<メモ>]
  if (parts.size() < 2) return usage(c);
  std::string name = trimStr(parts[1]);
  std::string memo = parts.size() >= 3 ? trimStr(parts[2]) : std::string();
  if (name.empty()) return errorResult(MsgId::ErrAnnEmptyName);
  cmd::SetAnnotation s;
  s.at = AddrSpec(trimStr(parts[0]));
  s.name = name;
  if (!memo.empty()) s.memo = memo;
  return posted(c, s);
}

ParseResult hMemo(Ctx c) {
  auto parts = splitComma(c.arg, 2); // <式>,<メモ>
  if (parts.size() < 2) return usage(c);
  cmd::SetAnnotation s;
  s.at = AddrSpec(trimStr(parts[0]));
  s.memo = trimStr(parts[1]); // 空メモで削除
  return posted(c, s);
}

ParseResult hUnann(Ctx c) {
  std::string a = trimStr(c.arg);
  if (a.empty()) return usage(c);
  cmd::RemoveAnnotation r;
  r.at = AddrSpec(a);
  return posted(c, r);
}

ParseResult hAnns(Ctx c) { return ui(action(UiAction::ShowAnnotations, trimStr(c.arg))); }

// ---- syscall ----

// "list [語|mach[:語]]" なら有効名の一覧（監視の有無・attach の有無に関係なく使える）。
bool syscallListArg(const std::string& a, std::string& filter) {
  if (!(a == "list" || a.rfind("list ", 0) == 0 || a.rfind("list\t", 0) == 0)) return false;
  size_t sp = a.find_first_of(" \t");
  filter = sp == std::string::npos ? std::string() : trimStr(a.substr(sp));
  return true;
}

ParseResult hScwatch(Ctx c) {
  std::string a = trimStr(c.arg), filter;
  if (syscallListArg(a, filter)) return ui(action(UiAction::ShowSyscallNames, filter));
  if (a == "off") return posted(c, cmd::SyscallWatchOff{});
  if (a.empty()) return ui(UiAction::ShowSyscallWatch); // 状況表示（対象終了後でも集計は残る）
  if (!c.dbg.attached()) return errorResult(MsgId::ErrNeedAttach);
  cmd::SyscallWatch w;
  w.filter = a;
  return posted(c, w);
}

ParseResult hScstop(Ctx c) {
  std::string a = trimStr(c.arg), filter;
  if (syscallListArg(a, filter)) return ui(action(UiAction::ShowSyscallNames, filter));
  if (a == "off") return posted(c, cmd::SyscallStopOff{});
  if (a.empty()) return ui(UiAction::ShowSyscallStop);
  if (!c.dbg.attached()) return errorResult(MsgId::ErrNeedAttach);
  cmd::SyscallStop s;
  s.filter = a;
  return posted(c, s);
}

// ---- 表示設定 / その他 ----

ParseResult hIntel(Ctx c) { cmd::SetDisasmSyntax s; s.intel = true; return posted(c, s); }
ParseResult hAtandt(Ctx c) { cmd::SetDisasmSyntax s; s.intel = false; return posted(c, s); }
ParseResult hBaseoff(Ctx) { UiAction a = action(UiAction::SetBaseRelative); a.flag = true; return ui(a); }
ParseResult hBaseon(Ctx) { UiAction a = action(UiAction::SetBaseRelative); a.flag = false; return ui(a); }
ParseResult hMouse(Ctx c) {
  if (c.arg == "on" || c.arg == "off") {
    UiAction a = action(UiAction::SetMouse);
    a.flag = (c.arg == "on");
    return ui(a);
  }
  return ui(UiAction::ToggleMouse); // 省略＝トグル
}
ParseResult hHelp(Ctx) { return ui(UiAction::ShowHelp); }

std::vector<CommandSpec> buildTable() {
  using G = CommandGroup;
  const unsigned A = CommandSpec::NeedsAttach, S = CommandSpec::NeedsStopped, B = CommandSpec::BatchOnly,
                 U = CommandSpec::UiOnly, T = CommandSpec::TakesArg, R = CommandSpec::Resumes,
                 X = CommandSpec::SlashSuffix;
  const MsgId N = MsgId::None;
  // name, aliases, completions, group, args, usage, help, flags, handler
  return {
    // --- 対象 ---
    {"attach",  {},          {"attach"},            G::Target, "[pid]",                 N, MsgId::HelpAttach,  T, hAttach},
    {"run",     {"launch"},  {"run", "launch"},     G::Target, "<path> [args...]",      MsgId::UsageRun, MsgId::HelpRun, T | R, hRun},
    {"restart", {"rerun"},   {"restart", "rerun"},  G::Target, "",                      N, MsgId::HelpRestart, R, hRestart},
    {"detach",  {},          {"detach"},            G::Target, "",                      N, MsgId::HelpDetach,  0, hDetach},
    {"q",       {"quit"},    {"quit"},              G::Target, "",                      N, MsgId::HelpQuit,    U, hQuit},
    {"exit",    {},          {},                    G::Target, "",                      N, MsgId::HelpQuit,    U | B, hQuit},
    // --- 実行 ---
    {"auto",    {},          {"auto"},              G::Run, "[call|syscall|sc|ret|all]...", N, MsgId::HelpAuto, S | T | R, hAuto},
    {"c",       {"continue"},                   {}, G::Run, "",                         N, MsgId::HelpContinue, S | B | R, hContinue},
    {"s",       {"si", "step", "stepi"},        {}, G::Run, "",                         N, MsgId::HelpStep,     S | B | R, hStep},
    {"n",       {"next"},                       {}, G::Run, "",                         N, MsgId::HelpNext,     S | B | R, hNext},
    {"wait",    {},                             {}, G::Run, "",                         N, MsgId::HelpWait,     U | B, hWait},
    // --- ブレイク ---
    {"b",       {},          {"b"},                 G::Breakpoint, "<expr>",            N, MsgId::HelpB,  A | T, hBreak},
    {"bh",      {},          {"bh"},                G::Breakpoint, "<expr>",            N, MsgId::HelpBh, A | T, hHwBreak},
    {"w",       {},          {"w"},                 G::Breakpoint, "<expr> [1|2|4|8] [r|w|rw]", MsgId::UsageW, MsgId::HelpW, A | T, hWatch},
    {"bl",      {},          {"bl"},                G::Breakpoint, "",                  N, MsgId::HelpBl, U, hBreakList},
    {"be",      {"bedit"},   {"be", "bedit"},       G::Breakpoint, "",                  N, MsgId::HelpBe, U, hBreakEdit},
    {"bd",      {},          {"bd"},                G::Breakpoint, "<#id|expr>",        MsgId::UsageBd, MsgId::HelpBd, T, hBreakDelete},
    {"bt",      {},          {"bt"},                G::Breakpoint, "<#id|expr>",        MsgId::UsageBt, MsgId::HelpBt, T, hBreakToggle},
    // --- 参照 ---
    {"reg",     {},          {"reg"},               G::Inspect, "[name | name=<expr> | xmmN | vN | vec | fp]", MsgId::UsageReg, MsgId::HelpReg, S | T, hReg},
    {"dr",      {},          {"dr"},                G::Inspect, "",                     N, MsgId::HelpDr, S | U, hDr},
    {"p",       {},          {"p"},                 G::Inspect, "[/x|/d] <expr>",       MsgId::UsageP, MsgId::HelpP, A | T | X, hPrint},
    {"x",       {},          {"x"},                 G::Inspect, "[/<count><fmt><size>] [<expr> [bytes]]", MsgId::UsageX, MsgId::HelpX, A | T | X, hExamine},
    {"dis",     {"disas", "disassemble"}, {"dis"},  G::Inspect, "[<expr> [count]]",     MsgId::UsageDisBatch, MsgId::HelpDis, A | T, hDis},
    {"sym",     {},          {"sym"},               G::Inspect, "<expr>",               N, MsgId::HelpSym, A | T, hSym},
    {"img",     {"libs"},    {"img", "libs"},       G::Inspect, "",                     N, MsgId::HelpImg, A | U, hImages},
    {"seg",     {},          {"seg"},               G::Inspect, "[image]",              N, MsgId::HelpSeg, A | U | T, hSegments},
    {"syms",    {},          {"syms"},              G::Inspect, "[image [filter]]",     N, MsgId::HelpSyms, A | U | T, hSymbols},
    {"uselib",  {},          {"uselib"},            G::Inspect, "[name|-]",             N, MsgId::HelpUselib, T, hUselib},
    {"where",   {"backtrace", "frames"},        {}, G::Inspect, "",                     N, MsgId::HelpWhere, S | U | B, hWhere},
    // --- 書き換え ---
    {"setmem",  {},          {"setmem"},            G::Modify, "<expr>,<hex bytes>",    MsgId::UsageSetmem, MsgId::HelpSetmem, A | T, hSetmemHex},
    {"setmem1", {"setmem2", "setmem4", "setmem8"}, {}, G::Modify, "<expr>,<value expr>", MsgId::UsageSetmem, MsgId::HelpSetmemN, A | T, hSetmemN},
    {"memset",  {},          {"memset"},            G::Modify, "<expr>,<value expr>,<size expr>", MsgId::UsageMemset, MsgId::HelpMemset, A | T, hMemset},
    {"wf",      {},          {"wf"},                G::Modify, "<expr>,<size expr>,<path>", MsgId::UsageFileIo, MsgId::HelpWf, A | T, hFileIo},
    {"rf",      {},          {"rf"},                G::Modify, "<expr>,<size expr>,<path>", MsgId::UsageFileIo, MsgId::HelpRf, A | T, hFileIo},
    // --- 注釈 ---
    {"ann",     {},          {"ann"},               G::Annotation, "<expr>,<name>[,<memo>]", MsgId::UsageAnn, MsgId::HelpAnn, A | T, hAnn},
    {"memo",    {},          {"memo"},              G::Annotation, "<expr>,<memo>",     MsgId::UsageMemo, MsgId::HelpMemo, A | T, hMemo},
    {"unann",   {},          {"unann"},             G::Annotation, "<expr>",            MsgId::UsageUnann, MsgId::HelpUnann, A | T, hUnann},
    {"anns",    {},          {"anns"},              G::Annotation, "[image]",           N, MsgId::HelpAnns, A | U | T, hAnns},
    // --- syscall ---
    {"scwatch", {},          {"scwatch"},           G::Syscall, "[<name>|all|mach[:<name>]|list [word]|off]", N, MsgId::HelpScwatch, T, hScwatch},
    {"scstop",  {},          {"scstop"},            G::Syscall, "[<name>|all|mach[:<name>]|list [word]|off]", N, MsgId::HelpScstop,  T, hScstop},
    // --- 表示設定 ---
    {"intel",   {},          {"intel"},             G::Display, "",                     N, MsgId::HelpIntel,   0, hIntel},
    {"atandt",  {"att"},     {"atandt"},            G::Display, "",                     N, MsgId::HelpAtandt,  0, hAtandt},
    {"baseoff", {},          {"baseoff"},           G::Display, "",                     N, MsgId::HelpBaseoff, U, hBaseoff},
    {"baseon",  {},          {"baseon"},            G::Display, "",                     N, MsgId::HelpBaseon,  U, hBaseon},
    {"mouse",   {},          {"mouse"},             G::Display, "[on|off]",             N, MsgId::HelpMouse,   U | T, hMouse},
    // --- その他 ---
    {"help",    {"h", "?"},  {"help"},              G::Misc, "",                        N, MsgId::HelpHelp,    U, hHelp},
  };
}

void jsonString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') { out += '\\'; out += (char)ch; }
    else if (ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); out += b; }
    else out += (char)ch;
  }
  out += '"';
}

void jsonStringArray(std::string& out, const std::vector<const char*>& v) {
  out += '[';
  for (size_t i = 0; i < v.size(); i++) { if (i) out += ','; jsonString(out, v[i]); }
  out += ']';
}

} // namespace

MsgId commandGroupTitle(CommandGroup g) {
  switch (g) {
    case CommandGroup::Target:     return MsgId::HelpGroupTarget;
    case CommandGroup::Run:        return MsgId::HelpGroupRun;
    case CommandGroup::Breakpoint: return MsgId::HelpGroupBreak;
    case CommandGroup::Inspect:    return MsgId::HelpGroupInspect;
    case CommandGroup::Modify:     return MsgId::HelpGroupModify;
    case CommandGroup::Annotation: return MsgId::HelpGroupAnnot;
    case CommandGroup::Syscall:    return MsgId::HelpGroupSyscall;
    case CommandGroup::Display:    return MsgId::HelpGroupDisplay;
    case CommandGroup::Misc:       return MsgId::HelpGroupMisc;
  }
  return MsgId::None;
}

const char* commandGroupKey(CommandGroup g) {
  switch (g) {
    case CommandGroup::Target:     return "target";
    case CommandGroup::Run:        return "run";
    case CommandGroup::Breakpoint: return "breakpoint";
    case CommandGroup::Inspect:    return "inspect";
    case CommandGroup::Modify:     return "modify";
    case CommandGroup::Annotation: return "annotation";
    case CommandGroup::Syscall:    return "syscall";
    case CommandGroup::Display:    return "display";
    case CommandGroup::Misc:       return "misc";
  }
  return "";
}

const std::vector<CommandSpec>& CommandInterpreter::table() {
  static const std::vector<CommandSpec> kTable = buildTable();
  return kTable;
}

const CommandSpec* CommandInterpreter::find(const std::string& name, FrontEndKind fe) {
  for (const CommandSpec& s : table()) {
    if ((s.flags & CommandSpec::BatchOnly) && fe != FrontEndKind::Batch) continue;
    if (name == s.name) return &s;
    for (const char* a : s.aliases) if (name == a) return &s;
    if (s.flags & CommandSpec::SlashSuffix) {
      std::string pre = std::string(s.name) + "/";
      if (name.rfind(pre, 0) == 0) return &s; // "x/4xg" "p/x"
    }
  }
  return nullptr;
}

Completion CommandInterpreter::complete(const std::string& input, FrontEndKind fe) {
  Completion out;
  out.replacement = input;
  if (input.find(' ') != std::string::npos) return out; // 最初の語のみ（引数部では何もしない）
  for (const CommandSpec& s : table()) {
    if ((s.flags & CommandSpec::BatchOnly) && fe != FrontEndKind::Batch) continue;
    for (const char* c : s.completions)
      if (std::string(c).rfind(input, 0) == 0) out.candidates.push_back(c); // 接頭辞一致
  }
  if (out.candidates.empty()) return out;
  if (out.candidates.size() == 1) {
    out.replacement = out.candidates[0];
    if (out.candidates[0] == "attach" || out.candidates[0] == "sym") out.replacement += " "; // v1 と同じ 2 つだけ空白を足す
    return out;
  }
  // 複数候補: 共通接頭辞まで伸ばす
  std::string common = out.candidates[0];
  for (size_t i = 1; i < out.candidates.size(); i++) {
    size_t k = 0;
    while (k < common.size() && k < out.candidates[i].size() && common[k] == out.candidates[i][k]) k++;
    common.resize(k);
  }
  if (common.size() > input.size()) out.replacement = common;
  return out;
}

std::string CommandInterpreter::catalogJson() {
  static const struct { unsigned bit; const char* name; } kFlags[] = {
    {CommandSpec::NeedsAttach, "needsAttach"}, {CommandSpec::NeedsStopped, "needsStopped"},
    {CommandSpec::BatchOnly, "batchOnly"},     {CommandSpec::UiOnly, "uiOnly"},
    {CommandSpec::TakesArg, "takesArg"},       {CommandSpec::Resumes, "resumes"},
    {CommandSpec::SlashSuffix, "slashSuffix"},
  };
  std::string out = "[";
  bool first = true;
  for (const CommandSpec& s : table()) {
    if (!first) out += ",";
    first = false;
    out += "\n  {\"name\":"; jsonString(out, s.name);
    out += ",\"aliases\":"; jsonStringArray(out, s.aliases);
    out += ",\"group\":"; jsonString(out, commandGroupKey(s.group));
    out += ",\"args\":"; jsonString(out, s.args);
    out += ",\"flags\":[";
    bool ff = true;
    for (const auto& f : kFlags)
      if (s.flags & f.bit) { if (!ff) out += ","; ff = false; jsonString(out, f.name); }
    out += "]";
    out += ",\"usageKey\":"; jsonString(out, msgKey(s.usage));
    out += ",\"helpKey\":"; jsonString(out, msgKey(s.help));
    out += "}";
  }
  out += "\n]\n";
  return out;
}

ParseResult CommandInterpreter::execute(const std::string& raw, FrontEndKind fe) {
  std::string line = raw;
  if (fe == FrontEndKind::Batch) {
    // バッチ: 空行・# コメント・':' 接頭辞（TUI と同じ綴り）を許容する（v1 batchExec）。
    size_t a = raw.find_first_not_of(" \t");
    if (a == std::string::npos) return ParseResult(); // 空行
    size_t b = raw.find_last_not_of(" \t");
    line = raw.substr(a, b - a + 1);
    if (line[0] == '#') return ParseResult();          // コメント
    if (line[0] == ':') line = line.substr(1);
    if (line.empty()) return ParseResult();
  }
  ParseResult r;
  // 非同期ジョブ（シンボル探索）実行中は次のコマンドを受けない（対象状態を同時に触らない）。
  if (dbg_.job().running) {
    r = errorResult(MsgId::ErrJobBusy);
    r.line = line;
    return r;
  }
  // コマンド実行前のこの境界で、イメージ一覧とユーザ注釈を最新化する（クエリで済むコマンドのため）。
  dbg_.refresh();

  std::string cmd = line, arg;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }

  const CommandSpec* spec = find(cmd, fe);
  if (!spec) {
    r = errorResult(Msg::of(MsgId::CmdUnknown).str("name", cmd));
    r.line = line;
    return r;
  }
  if ((spec->flags & CommandSpec::NeedsAttach) && !dbg_.attached()) {
    r = errorResult(MsgId::ErrNeedAttach);
  } else {
    CommandContext ctx{dbg_, *spec, cmd, arg, fe};
    r = spec->handler(ctx);
  }
  r.spec = spec;
  r.line = line;
  return r;
}

} // namespace engine
} // namespace ta2
