#pragma once

// L2 Facade/API（ta2engine）: 公開イベント（設計書 §7.1）。IEventSink::onEvent で届く。
#include <variant>

#include <sys/types.h>

#include "engine/Types.h"

namespace ta2 {
namespace engine {
namespace ev {

struct StateChanged { RunState state = RunState::Idle; pid_t pid = 0; };
struct Stopped { StopInfo info; };            // auto の途中経過・dlopen の内部停止は出さない
struct TargetExited {};
struct BreakpointsChanged {};                 // 一覧は Debugger::breakpoints() で取る
struct ImagesChanged { int pendingApplied = 0; bool internalStop = false; }; // 今回装着できた保留ブレイク数
struct Progress {
  enum Kind { Warm, Search } kind = Warm;     // シンボル先読み / シンボル探索ジョブ
  int done = 0, total = 0;
  bool active = true;                         // false = 完了（表示から消す）
};
struct CommandFinished { RequestId request = 0; CommandResult result; };
struct Notice { Msg message; };               // エンジン起点の通知（例: auto 中断）

} // namespace ev

using DebugEvent = std::variant<ev::StateChanged, ev::Stopped, ev::TargetExited, ev::BreakpointsChanged,
                                ev::ImagesChanged, ev::Progress, ev::CommandFinished, ev::Notice>;

// エンジン側（エンジンスレッド、またはインライン実行なら pump() を呼んだスレッド）から呼ばれる。
// 積むだけですぐ戻ること。呼び出し時にエンジンのロックは持っていないので、インライン実行では
// ここから Debugger のクエリを呼んでもよい（pump() の再入は不可）。
class IEventSink {
public:
  virtual ~IEventSink() = default;
  virtual void onEvent(const DebugEvent& ev) = 0;
};

} // namespace engine
} // namespace ta2
