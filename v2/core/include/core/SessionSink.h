#pragma once

// L1 Core（ta2core）: v1 Session の分割（設計書 §4.2 / §7.1）で、L1 の内部スレッドが
// 上位（L2 の EngineInbox）へ出来事を渡すための注入インタフェースと、停止の生データ RawStop。
//
// v1 は Session が app/EventQueue を直接握り Event::Stopped / TargetExited / Progress を push して
// いた（L1 が UI のイベント語彙を知っていた）。v2 ではこの SessionSink を上位が実装して渡す。
// L1 は「呼ぶ」だけで、積む先（EngineInbox）・後処理（StopProcessor）・公開イベント化は L2 の仕事。
//
// 実装側の約束（v1 の EventQueue::push と同じ性質を保つこと）:
//   - どの関数も「自分のキューへ積んで即戻る」だけにする。ここから Stepper / TargetSession を
//     同期的に呼び返してはいけない（onRawStop は Stepper の内部 mutex を握ったまま呼ばれ得る）。
//   - 呼び出しスレッドは関数ごとに違う（下記）。スレッドセーフに実装する。
#include <cstdint>
#include <string>

#include <mach/mach.h>

#include "base/Msg.h"

namespace ta2 {

// 1 回の停止の生データ（v1 Session の stop* メンバのスナップショット）。
// L2 の StopProcessor がこれを後処理して公開用の StopInfo を作る。文は含まない。
struct RawStop {
  thread_act_t thread = 0;      // 停止スレッド（syscall キャッチは暫定で先頭。refineStopThread で確定）
  uint64_t pc = 0;              // 停止 PC（ソフトブレイクは命令先頭へ補正済み）
  // 例外（クラッシュ）由来ならその種別（EXC_BAD_ACCESS 等）。通常のブレイク/ステップ停止は 0。
  exception_type_t exception = 0;
  int64_t code0 = 0;            // 例外 code[0]（BAD_ACCESS なら kern_return の種別）
  int64_t code1 = 0;            // 例外 code[1]（BAD_ACCESS ならフォルトしたアドレス）
  int watchId = -1;             // ウォッチ命中ならそのブレイク id、でなければ -1
  bool syscallCatch = false;    // syscall 停止キャッチポイント（:scstop）由来か
  std::string syscallName;      // そのとき発火した syscall / トラップ名（"write" / "mach:<名前>"）
  // dyld のイメージ通知フック命中（dlopen 等）の内部停止か。true なら上位は公開せず、
  // イメージ更新＋保留ブレイク装着をして即 cont する（ユーザには止まって見えない）。
  bool imageLoad = false;
  // 例外スレッドが resume 決定を待っていない停止か（Ctrl+C / dtrace stop() / エントリ不明の launch）。
  // 上位は気にしなくてよい（Stepper::cont 等が内部で経路を選ぶ）。診断用。
  bool manual = false;
};

// RawStop → 停止理由（v1 Session::stopReason の構造化版）。通常のブレイク/ステップ停止なら
// 空の Msg（empty()）。EXC_* / KERN_* の識別子は翻訳対象外なので引数にそのまま入る。
Msg stopReasonMsg(const RawStop& s);

class SessionSink {
public:
  virtual ~SessionSink() = default;

  // 対象が停止した（v1 Event::Stopped）。呼び出しスレッド: 例外サーバ / dtrace consumer /
  // interrupt() や launch() を呼んだスレッド。受け手は後で Stepper::cont / stepInto / stepOver
  // のいずれかを呼んで再開を決める（それが「resume 決定」）。
  virtual void onRawStop(const RawStop& stop) = 0;

  // 対象プロセスが終了した（v1 Event::TargetExited）。呼び出しスレッド: 終了監視（ExitMonitor）。
  virtual void onTargetExited() = 0;

  // シンボル先読みの進捗（v1 Event::Progress）。開始時・80ms 間引きの途中経過・完了時に呼ぶ。
  // 呼び出しスレッド: 先読み（SymbolWarmer）と、開始を呼んだスレッド。active=false が完了。
  virtual void onWarmProgress(int done, int total, bool active) = 0;
};

} // namespace ta2
