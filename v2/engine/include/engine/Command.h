#pragma once

// L2 Facade/API（ta2engine）: エンジンへの全操作（設計書 §6.2）。Debugger::post() へ渡す。
// 構造体の std::variant。アドレスを取るものは数値でも式でも渡せる（AddrSpec）。
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <sys/types.h>

#include "engine/Types.h"

namespace ta2 {
namespace engine {
namespace cmd {

struct Attach   { pid_t pid = 0; };
struct Launch   { std::vector<std::string> args; };  // args[0] = 実行ファイルパス
struct Restart  {};                                  // 同じ引数で起動し直し、ブレイクを引き継ぐ
struct Detach   {};
struct Continue {};
struct StepInto {};
struct StepOver {};
struct Auto     { unsigned mask = 7; };              // 1=call 2=syscall 4=ret

struct AddBreakpoint      { AddrSpec at; BpKind kind = BpKind::Soft; WatchParams watch; };
struct RemoveBreakpoint   { BpRef ref; };
struct ToggleEnabled      { BpRef ref; };
struct ToggleBreakpointAt { uint64_t addr = 0; };    // b キー / クリック。あれば解除、無ければソフトで設定
// 種別変換・移動・ウォッチ条件変更。失敗時は元の種別・アドレスへ巻き戻す（id は変わる）。
struct RebuildBreakpoint  { int id = 0; BpKind kind = BpKind::Soft; uint64_t addr = 0; WatchParams watch; };
struct SaveBreakpoints    {};

struct SetRegister  { std::string name; AddrSpec value; };
struct WriteMemory  { AddrSpec at; std::vector<uint8_t> bytes; };            // setmem <式>,<HEX列>
struct WriteValue   { AddrSpec at; AddrSpec value; int size = 8; };          // setmem1/2/4/8（リトルエンディアン）
struct FillMemory   { AddrSpec at; AddrSpec value; AddrSpec size; };         // memset
struct DumpToFile   { AddrSpec at; AddrSpec size; std::string path; };       // wf
struct LoadFromFile { AddrSpec at; AddrSpec size; std::string path; };       // rf

// name あり: 名前を付ける（memo が非空ならメモも）。name なし: メモだけ設定（空メモで削除）。
struct SetAnnotation    { AddrSpec at; std::optional<std::string> name; std::optional<std::string> memo; };
struct RemoveAnnotation { AddrSpec at; };
// m キー: 未登録なら func<0 詰め 16 進オフセット> で登録、登録済みなら既存内容を返す（AnnotationEdit）。
struct AutoAnnotate     { uint64_t addr = 0; };
struct SetDefaultLib    { std::string name; };       // 空で解除

struct SyscallWatch    { std::string filter; };      // ""/"all" で全件集計、名前でトレース
struct SyscallWatchOff {};
struct SyscallStop     { std::string filter; };
struct SyscallStopOff  {};

struct Evaluate { std::string expr; };               // :p / :dis の移動先。結果は CommandResult::value
struct Describe { AddrSpec at; };                    // :sym。value + where
// :x。spec が空なら従来の 16 進ダンプ（classicBytes バイト）、あれば x/NFS。結果は payload の ExamineResult。
struct Examine  { AddrSpec at; std::string spec; int classicBytes = 64; };
struct SetDisasmSyntax { bool intel = false; };

} // namespace cmd

using Command = std::variant<
  cmd::Attach, cmd::Launch, cmd::Restart, cmd::Detach,
  cmd::Continue, cmd::StepInto, cmd::StepOver, cmd::Auto,
  cmd::AddBreakpoint, cmd::RemoveBreakpoint, cmd::ToggleEnabled, cmd::ToggleBreakpointAt,
  cmd::RebuildBreakpoint, cmd::SaveBreakpoints,
  cmd::SetRegister, cmd::WriteMemory, cmd::WriteValue, cmd::FillMemory, cmd::DumpToFile, cmd::LoadFromFile,
  cmd::SetAnnotation, cmd::RemoveAnnotation, cmd::AutoAnnotate, cmd::SetDefaultLib,
  cmd::SyscallWatch, cmd::SyscallWatchOff, cmd::SyscallStop, cmd::SyscallStopOff,
  cmd::Evaluate, cmd::Describe, cmd::Examine, cmd::SetDisasmSyntax>;

} // namespace engine
} // namespace ta2
