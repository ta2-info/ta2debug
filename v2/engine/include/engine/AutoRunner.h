#pragma once

// L2（ta2engine）: auto（興味のある命令まで自動継続）の継続判定。由来は v1 main.cpp の
// g_autoMask / g_autoSteps / kindAtPc / kAutoMax と onStoppedEvent の auto 分岐（設計書 §4.3）。
// 停止のたびに、現命令が該当（call / syscall / ret）・ブレイク命中・例外・上限なら止め、
// そうでなければ「さらに 1 命令」を返す。実際の stepInto は呼び出し側（StopProcessor）が行う。
#include <cstdint>

#include "core/TargetSession.h"
#include "engine/Types.h"

namespace ta2 {
namespace engine {

class AutoRunner {
public:
  static constexpr long kAutoMax = 2000000; // 暴走防止の上限（v1 kAutoMax）
  enum MaskBit : unsigned { Call = 1, Syscall = 2, Return = 4 };

  // auto 開始（mask のビット: 1=call 2=syscall 4=ret）。最初の stepInto は呼び出し側。
  void start(unsigned mask) { mask_ = mask; steps_ = 0; interrupted_ = false; }
  bool active() const { return mask_ != 0; }
  // 中断要求（Ctrl+C）。auto 中なら打ち切って true（実行中の 1 ステップが次に止まった所で確定停止）。
  bool interrupt();
  // 対象を手放したとき。auto 状態を次の対象へ持ち越さない。
  void reset() { mask_ = 0; steps_ = 0; interrupted_ = false; }

  struct Verdict {
    bool wasAuto = false;    // この停止が auto に属していたか
    bool keepGoing = false;  // true: まだ興味なし → 呼び出し側が stepInto して次の停止を待つ
    AutoStop why = AutoStop::None; // 停止確定の理由（ブレイク命中・例外で止まったときは None）
    long steps = 0;          // 今回の auto で踏んだステップ数
  };
  // 停止ごとに呼ぶ（停止中・決定者のスレッドから）。
  Verdict onStop(TargetSession& session);

  // 停止位置の命令種別（ブレイク命令バイトは元に戻して判定）。v1 kindAtPc。
  static InsnKind kindAtPc(TargetSession& session, uint64_t pc);

private:
  unsigned mask_ = 0;
  long steps_ = 0;
  bool interrupted_ = false;
};

} // namespace engine
} // namespace ta2
