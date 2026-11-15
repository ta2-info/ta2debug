// L3 Presentation: ta2ai — 非対話（バッチ / AI エージェント）正面。v1 の `-b -o` の発展（設計書 §7.5）。
// ヘッドレスな Debugger（インライン実行）を CommandInterpreter で駆動し、結果・停止・バックトレースを
// TextReport + Translator でテキスト化する。TUI と同じエンジン・同じ目録を使う第 3 のレンダラ。
// 固定トークン（"ta2> " / "stopped @ " / "backtrace:" / "src: "）は英語のまま（§8.7）。
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <unistd.h>

#include "core/SymbolRef.h" // formatDescribe（:sym の記述）
#include "engine/CommandInterpreter.h"
#include "engine/Debugger.h"
#include "engine/Event.h"
#include "i18n/Translator.h"
#include "ui/TextReport.h"

using namespace ta2;
using namespace ta2::engine;

namespace {

// エンジンイベントを受け、コマンド結果・停止・終了・通知を溜める（インライン実行なので pump() から呼ばれる）。
struct BatchSink : IEventSink {
  std::unordered_map<RequestId, CommandResult> finished;
  bool sawStop = false, sawExit = false;
  StopInfo stop;
  std::vector<Msg> notices;

  void onEvent(const DebugEvent& ev) override {
    if (auto e = std::get_if<ev::CommandFinished>(&ev)) finished[e->request] = e->result;
    else if (auto s = std::get_if<ev::Stopped>(&ev)) { sawStop = true; stop = s->info; }
    else if (std::get_if<ev::TargetExited>(&ev)) sawExit = true;
    else if (auto n = std::get_if<ev::Notice>(&ev)) notices.push_back(n->message);
  }
};

void emit(const std::vector<std::string>& lines) {
  for (const auto& l : lines) { fwrite(l.data(), 1, l.size(), stdout); fputc('\n', stdout); }
  fflush(stdout);
}
void emit1(const std::string& l) { fwrite(l.data(), 1, l.size(), stdout); fputc('\n', stdout); fflush(stdout); }

class BatchApp {
public:
  BatchApp()
    : dbg_(EngineOptions::batch()), tr_(loadCatalog(detectLocaleLang()), detectLocaleLang()), interp_(dbg_), report_(tr_, dbg_) {
    sub_ = dbg_.subscribe(&sink_);
  }
  ~BatchApp() { dbg_.unsubscribe(sub_); }

  // 1 行実行。false を返したら終了（quit）。
  bool exec(const std::string& raw) {
    ParseResult pr = interp_.execute(raw, FrontEndKind::Batch);
    if (pr.kind == ParseResult::Empty) return true;         // 空行・コメント
    emit1("ta2> " + pr.line);

    switch (pr.kind) {
      case ParseResult::Error:
        emit1(tr_.tr(pr.error));
        return true;
      case ParseResult::Ui:
        return handleUi(pr.action, pr.spec);
      case ParseResult::Posted: {
        if (pr.action.kind == UiAction::ShowExamine) lastExSpec_ = parseExSpec(pr.line); // :x の書式を覚える
        CommandResult res = waitFinished(pr.request);
        emit(report_.commandResult(res));
        handleFollowup(pr.action, res);
        drainNotices();
        if (pr.spec && (pr.spec->flags & CommandSpec::Resumes) && res.ok) waitAndReport();
        return true;
      }
      default:
        return true;
    }
  }

private:
  // Posted の CommandFinished が来るまで受信箱を回す。
  CommandResult waitFinished(RequestId id) {
    while (sink_.finished.find(id) == sink_.finished.end()) {
      if (!dbg_.pumpWait(5000)) break; // 5 秒来なければ諦める（通常は即来る）
    }
    auto it = sink_.finished.find(id);
    CommandResult r = (it == sink_.finished.end()) ? CommandResult{} : it->second;
    if (it != sink_.finished.end()) sink_.finished.erase(it);
    return r;
  }

  // 停止 or 対象終了まで待ってレポート。
  void waitAndReport() {
    sink_.sawStop = sink_.sawExit = false;
    while (!sink_.sawStop && !sink_.sawExit) {
      if (!dbg_.pumpWait(-1)) break;
    }
    drainNotices();
    if (sink_.sawExit) { emit1(tr_.tr(Msg::of(MsgId::UiTargetExited))); return; }
    if (sink_.sawStop) emit(report_.stopReport(sink_.stop, dbg_.backtrace()));
  }

  void drainNotices() {
    for (auto& m : sink_.notices) { std::string s = tr_.tr(m); if (!s.empty()) emit1(s); }
    sink_.notices.clear();
  }

  // Posted 完了後のフォローアップ（p / sym / x の表示）。
  void handleFollowup(const UiAction& a, const CommandResult& res) {
    switch (a.kind) {
      case UiAction::ShowValue: {
        if (!res.value) break;
        uint64_t v = *res.value; char b[96];
        if (a.format == 'x') snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
        else if (a.format == 'd') snprintf(b, sizeof(b), "%lld", (long long)v);
        else snprintf(b, sizeof(b), "0x%llx (%lld)", (unsigned long long)v, (long long)v);
        emit1(a.text + " = " + b);
        break;
      }
      case UiAction::ShowSymbol:
        // 記述（image off func+0xNN）は commandResult が res.where から既に出す。
        // シンボルが無いときだけ補う。
        if (!(res.where && res.where->hasImage)) emit1(tr_.tr(Msg::of(MsgId::UiSymNoneAt)));
        break;
      case UiAction::ShowExamine: {
        if (auto ex = std::get_if<ExamineResult>(&res.payload)) { lastExAddr_ = ex->nextAddr; haveLastEx_ = true; emit(report_.examine(*ex)); }
        break;
      }
      default: break;
    }
  }

  // 表示側で完結するコマンド（参照系）。false で終了。
  bool handleUi(const UiAction& a, const CommandSpec*) {
    switch (a.kind) {
      case UiAction::Quit: return false;
      case UiAction::ShowRegisters: emit(report_.registers(dbg_.registers())); break;
      case UiAction::ShowRegister: {
        uint64_t v = 0;
        if (dbg_.readRegister(a.text, v)) { char b[32]; snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); emit1(a.text + " = " + b); }
        else emit1(tr_.tr(Msg::of(MsgId::UiUnknownRegister).str("name", a.text)));
        break;
      }
      case UiAction::ShowVectorRegisters: emit(report_.vectorRegisters(dbg_.vectorRegisters(), a.text)); break;
      case UiAction::ShowDebugRegisters: emit(report_.debugRegisters(dbg_.debugRegisters())); break;
      case UiAction::ShowSyscallWatch: emit(report_.syscallWatch(dbg_.syscallWatch())); break;
      case UiAction::ShowSyscallStop: emit(report_.syscallStop(dbg_.syscallWatch())); break;
      case UiAction::ShowSyscallNames: {
        SyscallNameList ns; Msg err;
        if (Debugger::syscallNames(a.text, ns, err)) emit(report_.syscallNames(ns));
        else emit1(tr_.tr(err));
        break;
      }
      case UiAction::SetBaseRelative: emit1(tr_.tr(Msg::of(a.flag ? MsgId::UiBaseoffOn : MsgId::UiBaseoffOff))); break;
      case UiAction::ContinueExamine: continueExamine(a.text); break;
      case UiAction::ShowBreakpoints: emit(report_.breakpoints(dbg_.breakpoints())); break;
      case UiAction::ShowImages: emit(report_.images(dbg_.images())); break;
      case UiAction::ShowSegments: emit(report_.segments(dbg_.segments(a.text))); break;
      case UiAction::ShowSymbols: emit(report_.symbols(dbg_.symbols(a.text, a.text2, 0))); break;
      case UiAction::ShowAnnotations: emit(report_.annotations(dbg_.annotations(a.text))); break;
      case UiAction::ShowHelp: emit(report_.help()); break;
      case UiAction::ShowDefaultLib: { std::string d = dbg_.defaultLib();
        emit1(d.empty() ? tr_.tr(Msg::of(MsgId::UiDefaultLibAuto)) : tr_.tr(Msg::of(MsgId::UiDefaultLib).str("name", d))); break; }
      case UiAction::PrintBacktrace:
        if (dbg_.state() == RunState::Stopped) emit(report_.backtrace(dbg_.backtrace()));
        else emit1(tr_.tr(Msg::of(MsgId::UiNotStopped)));
        break;
      case UiAction::WaitForStop:
        if (dbg_.state() == RunState::Running) waitAndReport();
        else emit1(tr_.tr(Msg::of(MsgId::UiNotRunning)));
        break;
      case UiAction::FollowPc: {
        if (dbg_.state() != RunState::Stopped) { emit1(tr_.tr(Msg::of(MsgId::UiNotStopped))); break; }
        DisasmResult dr = dbg_.disassemble(0, 20);
        ExamineResult ex; Msg err;
        if (dbg_.examine(std::to_string(20) + "i", dr.pc, ex, err)) emit(report_.examine(ex));
        break;
      }
      default:
        emit1(tr_.tr(Msg::of(MsgId::UiCmdUnsupported)));
        break;
    }
    return true;
  }

  // "x/8xb 式" / "x /8xb 式" → 書式 "8xb"（hExamine と同じ規則）。無ければ空。
  static std::string parseExSpec(const std::string& line) {
    size_t i = line.find_first_not_of(" \t"); if (i == std::string::npos) return "";
    size_t j = line.find_first_of(" \t", i); std::string cmd = line.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (cmd.size() > 2 && cmd[0] == 'x' && cmd[1] == '/') return cmd.substr(2);
    if (j == std::string::npos) return "";
    size_t k = line.find_first_not_of(" \t", j); if (k == std::string::npos) return "";
    size_t l = line.find_first_of(" \t", k); std::string t = line.substr(k, l == std::string::npos ? std::string::npos : l - k);
    return (!t.empty() && t[0] == '/') ? t.substr(1) : std::string();
  }
  void continueExamine(const std::string& override) {
    if (!haveLastEx_) { emit1(tr_.tr(Msg::of(MsgId::UiExamineNoPrev))); return; }
    std::string spec = override.empty() ? lastExSpec_ : override;
    ExamineResult ex; Msg err;
    bool ok = spec.empty() ? dbg_.classicDump(lastExAddr_, 64, ex, err) : dbg_.examine(spec, lastExAddr_, ex, err);
    if (!ok) { emit1(tr_.tr(err)); return; }
    lastExAddr_ = ex.nextAddr; lastExSpec_ = spec; haveLastEx_ = true;
    emit(report_.examine(ex));
  }

  Debugger dbg_;
  Translator tr_;
  CommandInterpreter interp_;
  BatchSink sink_;
  ui::TextReport report_;
  SubscriptionId sub_ = 0;
  std::string lastExSpec_; uint64_t lastExAddr_ = 0; bool haveLastEx_ = false; // :x 継続
};

void printUsage() {
  fputs(
    "ta2ai — ta2debug 非対話（バッチ / AI）正面（自作プログラム専用）\n"
    "使い方:\n"
    "  ta2ai [対象] -o \"cmd\" [-o ...]     -o のコマンドを順に実行\n"
    "  ta2ai [対象] -s スクリプト          ファイルの各行を実行（- で標準入力）\n"
    "  ta2ai [対象]                        標準入力からコマンドを読む\n"
    "  対象 = pid（全数字）または パス[＋引数]\n",
    stdout);
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> ocmds, target;
  std::string scriptPath; bool haveScript = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-b" || a == "--batch") continue;            // v1 互換（常にバッチ）
    else if (a == "-o") { if (i + 1 < argc) ocmds.push_back(argv[++i]); }
    else if (a == "-s" || a == "--source") { if (i + 1 < argc) { scriptPath = argv[++i]; haveScript = true; } }
    else if (a == "-h" || a == "--help") { printUsage(); return 0; }
    else target.push_back(a);
  }

  if (geteuid() != 0) { fprintf(stderr, "root 権限で実行してください\n"); return 1; }

  std::vector<std::string> script;
  if (!target.empty()) {
    bool isPid = !target[0].empty();
    for (char ch : target[0]) if (ch < '0' || ch > '9') isPid = false;
    if (isPid) script.push_back("attach " + target[0]);
    else { std::string r = "run"; for (auto& t : target) r += " " + t; script.push_back(r); }
  }
  for (auto& c : ocmds) script.push_back(c);
  if (haveScript) {
    FILE* f = (scriptPath == "-") ? stdin : fopen(scriptPath.c_str(), "r");
    if (!f) { fprintf(stderr, "スクリプトを開けない: %s\n", scriptPath.c_str()); return 1; }
    char buf[2048];
    while (fgets(buf, sizeof(buf), f)) { std::string l(buf); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); script.push_back(l); }
    if (f != stdin) fclose(f);
  }
  bool fromStdin = ocmds.empty() && !haveScript;

  BatchApp app;
  for (auto& l : script) if (!app.exec(l)) return 0;
  if (fromStdin) {
    char buf[2048];
    while (fgets(buf, sizeof(buf), stdin)) { std::string l(buf); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); if (!app.exec(l)) break; }
  }
  return 0;
}
