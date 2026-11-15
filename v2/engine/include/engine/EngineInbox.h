#pragma once

// L2（ta2engine）: 内部スレッド（例外・終了監視・先読み・dtrace・ジョブ）と post() → エンジンの受信箱。
// 由来は v1 app/EventQueue のうち Stopped / TargetExited / Progress / JobDone（設計書 §4.3 / §7.1）。
// UI の語彙（Key / Mouse / Resize）は持たない。
//
// SessionSink の実装でもある。L1 の約束どおり onRawStop / onTargetExited / onWarmProgress は
// 「自分のキューへ積んで即戻る」だけで、Stepper / TargetSession を呼び返さない（onRawStop は
// Stepper の mutex を握ったまま呼ばれ得る。ここで取るのは受信箱の mutex だけ）。
//
// 世代（generation）: 対象を手放す（detach）たびにエンジンが進める。RawStop / TargetExited は積んだ
// 時点の世代を持ち、処理時に古ければ捨てる（前の対象の停止・終了が次の対象に誤適用されない）。
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include "core/SessionSink.h"
#include "engine/Command.h"

namespace ta2 {
namespace engine {

struct InboxItem {
  enum Kind { Command, RawStop, TargetExited, WarmProgress, JobProgress, JobDone, Interrupt, Quit };
  Kind kind = Quit;
  uint64_t generation = 0;
  RequestId request = 0;       // Command
  engine::Command command;     // Command
  ta2::RawStop stop;           // RawStop
  int done = 0, total = 0;     // WarmProgress / JobProgress
  bool active = false;         // WarmProgress
};

class EngineInbox : public SessionSink {
public:
  // ---- SessionSink（L1 の内部スレッドから。積むだけ） ----
  void onRawStop(const ta2::RawStop& stop) override;
  void onTargetExited() override;
  void onWarmProgress(int done, int total, bool active) override;

  void push(InboxItem item);       // 末尾へ（世代は現在値を刻む）
  void pushFront(InboxItem item);  // 先頭へ（保留していた項目の差し戻し。世代は項目のまま）
  // 1 件取り出す。timeoutMs < 0: 来るまで待つ / 0: 待たない / >0: その時間まで待つ。無ければ false。
  bool pop(InboxItem& out, int timeoutMs);

  uint64_t generation() const { return generation_.load(); }
  void advanceGeneration() { generation_.fetch_add(1); }

private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<InboxItem> q_;
  std::atomic<uint64_t> generation_{1};
};

} // namespace engine
} // namespace ta2
