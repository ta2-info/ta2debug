#pragma once

// L2（ta2engine）: 生の停止（RawStop）の後処理。由来は v1 main.cpp の onStoppedEvent と
// メインループの Stopped 分岐（設計書 §4.3 / §7.3）。
//   - dlopen の内部停止（RawStop::imageLoad）は公開せず、イメージ更新 + 保留ブレイク装着をして即 cont。
//   - 通常の停止は v1 と同じ順序で後処理する:
//       refreshImages → refreshAnnotations → syscalls().refineStopThread() → warmSymbols() → dyld().ensure()
//   - auto 継続中なら AutoRunner に判定させ、継続なら stepInto して公開しない。
//   - 停止が確定したら StopInfo を作り、起動直後の一度きりの保存ブレイク復元と保留ブレイク装着を行う。
// 公開イベント化（Stopped / ImagesChanged / StateChanged の発行）は呼び出し側の Debugger が行う。
// エンジン（決定者）のスレッドからのみ呼ぶ。
#include "core/SessionSink.h"
#include "core/TargetSession.h"
#include "engine/AutoRunner.h"
#include "engine/BreakpointService.h"
#include "engine/Types.h"

namespace ta2 {
namespace engine {

class StopProcessor {
public:
  StopProcessor(TargetSession& session, BreakpointService& bps, AutoRunner& autoRunner,
                const EngineOptions& opts)
    : s_(session), bps_(bps), auto_(autoRunner), opts_(opts) {}

  // ---- Debugger が状況を知らせる（StopKind の分類と一度きりの復元に使う） ----
  enum class Resume { None, Continue, StepInto, StepOver };
  // launch 成功時。restoreSaved=true なら次の確定停止で保存ブレイクを一度だけ復元する。
  void noteLaunch(bool restoreSaved) { entryPending_ = true; restorePending_ = restoreSaved; lastResume_ = Resume::None; }
  void noteAttach() { entryPending_ = false; restorePending_ = false; lastResume_ = Resume::None; }
  void noteResume(Resume r) { lastResume_ = r; }
  void reset() { entryPending_ = false; restorePending_ = false; lastResume_ = Resume::None; }

  struct Outcome {
    enum Kind {
      Dropped,        // 対象が無い / 停止していない（古い通知）。何もしていない
      Internal,       // dlopen の内部停止。処理して cont 済み（ユーザには止まって見えない）
      AutoContinued,  // auto 継続。stepInto 済み（まだ Running）
      Stopped,        // 停止確定。info が有効
    } kind = Dropped;
    bool imagesChanged = false;      // イメージ一覧を読み直した
    int pendingApplied = 0;          // 今回装着できた保留ブレイク数
    bool breakpointsChanged = false; // 復元・装着でブレイク一覧が変わった
    StopInfo info;
  };
  Outcome process(const RawStop& raw);

private:
  StopInfo buildStopInfo(const AutoRunner::Verdict& av);

  TargetSession& s_;
  BreakpointService& bps_;
  AutoRunner& auto_;
  const EngineOptions& opts_;
  bool entryPending_ = false;    // launch 後、まだ最初の確定停止（エントリ）を見ていない
  bool restorePending_ = false;  // 次の確定停止で保存ブレイクを復元する（v1 g_restorePending）
  Resume lastResume_ = Resume::None;
};

} // namespace engine
} // namespace ta2
