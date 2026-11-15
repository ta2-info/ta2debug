#pragma once

// L2 Facade/API（ta2engine）: テキスト 1 行 → Command（エンジン向け）または UiAction（表示側で
// 完結する要求）。由来は v1 main.cpp の runCommand / completeCommand（CMDS[]）/ withBreakId /
// batchExec のバッチ専用コマンド（設計書 §4.3 / §6.4）。
//
// コマンド表（CommandSpec の配列）が、解釈・補完・ヘルプ・使用法の唯一の出所。
// 機械可読な目録（catalogJson）もこの表から生成する（自己記述的コマンド目録。§7.5）。
// UiAction は「何を表示・操作するか」の種別と引数を返すだけで、中身の実行（整形・ペイン操作・
// 停止待ち）は L3 が Debugger のクエリを使って行う。
#include <string>
#include <vector>

#include "base/Msg.h"
#include "engine/Debugger.h"
#include "engine/Types.h"

namespace ta2 {
namespace engine {

enum class FrontEndKind { Tui, Batch };

enum class CommandGroup { Target, Run, Breakpoint, Inspect, Modify, Annotation, Syscall, Display, Misc };
// ヘルプの見出し（help.group.*）。
MsgId commandGroupTitle(CommandGroup g);
// 目録用の識別子（"target" / "run" / ...。翻訳対象外）。
const char* commandGroupKey(CommandGroup g);

// 表示側だけで完結する要求（エンジンは関与しない）。Posted の完了後に行うフォローアップにも使う。
struct UiAction {
  enum Kind {
    None,
    Quit,                 // :q / :quit（バッチは exit も）
    OpenProcessPicker,    // :attach（引数なし）
    OpenBreakpointEditor, // :be / :bedit
    ShowHelp,             // :help（CommandInterpreter::table() から生成）
    ShowBreakpoints,      // :bl（breakpoints()）
    ShowRegisters,        // :reg（registers()）
    ShowRegister,         // :reg <名前>（text = 名前。readRegister()）
    ShowVectorRegisters,  // :reg xmm[N] / vN / vec / fp（text = 1 本の名前。空なら全部）
    ShowDebugRegisters,   // :dr（debugRegisters()）
    ShowImages,           // :img / :libs（images()）
    ShowSegments,         // :seg [名前]（text = イメージ名。segments()）
    ShowSymbols,          // :syms [ライブラリ] [絞込]（text / text2。symbols()）
    ShowAnnotations,      // :anns [ライブラリ]（text。annotations()）
    ShowDefaultLib,       // :uselib（引数なし。defaultLib()）
    ShowSyscallWatch,     // :scwatch（引数なし。syscallWatch()）
    ShowSyscallStop,      // :scstop（引数なし。syscallWatch()）
    ShowSyscallNames,     // :scwatch list [語] / :scstop list [語]（text = 絞込。syscallNames()）
    ContinueExamine,      // :x（式なし。前回の続き。text = 書式の上書き、空なら前回のまま）
    FollowPc,             // :dis（引数なし。ペインを現在 PC へ戻す）
    SetMouse,             // :mouse on / off（flag）
    ToggleMouse,          // :mouse
    SetBaseRelative,      // :baseoff（flag=true）/ :baseon（flag=false）
    WaitForStop,          // バッチ: 停止（または対象終了）を待って停止レポートを出す
    PrintBacktrace,       // バッチ: where / backtrace / frames
    // ---- Posted の完了（CommandFinished ok）後に行うフォローアップ ----
    ShowValue,            // :p（text = 式、format = 'x' / 'd' / 0。CommandResult::value）
    ShowSymbol,           // :sym（value + where）
    ShowExamine,          // :x / バッチの :dis（payload の ExamineResult。続きは nextAddr から）
    GotoDisasm,           // TUI の :dis <式>（value へペインを移動。完了時に Stopped を再確認すること）
  } kind = None;
  std::string text;
  std::string text2;
  char format = 0;
  bool flag = false;
};

struct CommandSpec;

struct ParseResult {
  enum Kind {
    Empty,   // 空行・コメント（バッチ）。何もしない
    Posted,  // エンジンへ post 済み。結果は CommandFinished{request}。action は完了後のフォローアップ
    Ui,      // 表示側で実行する（action）
    Error,   // 解釈・事前条件の誤り（error）
  } kind = Empty;
  RequestId request = 0;
  UiAction action;
  Msg error;
  const CommandSpec* spec = nullptr; // 一致したコマンド（不明なら nullptr）
  std::string line;                  // 前処理後の 1 行（バッチのエコー "ta2> " 用）
};

struct CommandContext; // ハンドラへ渡す文脈（実体は .cpp）

struct CommandSpec {
  enum Flag : unsigned {
    NeedsAttach  = 1u << 0, // 未アタッチなら err.cmd.need_attach（ハンドラより先に判定）
    NeedsStopped = 1u << 1, // 停止中のみ（文はコマンドごとに違うのでハンドラが返す。目録用の印）
    BatchOnly    = 1u << 2, // バッチでだけ使える（TUI では不明なコマンド）
    UiOnly       = 1u << 3, // 表示側だけで完結する（エンジンへは何も post しない）
    TakesArg     = 1u << 4, // 引数を取る
    Resumes      = 1u << 5, // 対象を走らせる（バッチは続けて停止を待つ）
    SlashSuffix  = 1u << 6, // "x/4xg" "p/x" のように名前の直後へ /書式 を付けられる
  };
  const char* name;                       // "b"
  std::vector<const char*> aliases;       // {"bedit"} など
  std::vector<const char*> completions;   // TAB 補完に出す綴り（v1 CMDS[] と同じ集合）
  CommandGroup group;
  const char* args;                       // 引数の構文（"<expr> [1|2|4|8] [r|w|rw]"。翻訳対象外）
  MsgId usage;                            // 使い方 1 行（v1 に文があるものだけ。無ければ MsgId::None）
  MsgId help;                             // 1 行説明（help.cmd.*）
  unsigned flags;
  ParseResult (*handler)(const CommandContext&);
};

struct Completion {
  std::vector<std::string> candidates;    // 接頭辞一致した綴り（表の順）
  std::string replacement;                // 入力行の置き換え（単一なら確定、複数なら共通接頭辞。変化なしなら入力のまま）
};

class CommandInterpreter {
public:
  explicit CommandInterpreter(Debugger& dbg) : dbg_(dbg) {}

  // コマンド表（補完・ヘルプ・使用法・目録の唯一の出所）。
  static const std::vector<CommandSpec>& table();
  // 名前または別名で引く（"x/4xg" "p/x" は SlashSuffix の本体に一致）。fe で使えないものは nullptr。
  static const CommandSpec* find(const std::string& name, FrontEndKind fe);
  // コマンド名の補完（最初の語のみ。空白を含む入力は候補なし・置き換えなし）。v1 completeCommand。
  static Completion complete(const std::string& input, FrontEndKind fe = FrontEndKind::Tui);
  // 機械可読なコマンド目録（JSON 配列。name / aliases / group / args / flags / usageKey / helpKey）。
  static std::string catalogJson();

  // 1 行を解釈する。エンジン向けは post して RequestId を、表示向けは UiAction を、誤りは Msg を返す。
  ParseResult execute(const std::string& line, FrontEndKind fe);

private:
  Debugger& dbg_;
};

} // namespace engine
} // namespace ta2
