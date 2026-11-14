#pragma once

// L1 Core（ta2core）: 「止まる / 続ける / ステップ」の状態機械。由来は v1 Session の
// onException / applyResume / decide / resumeManual / interrupt / setStep / detectWatchHit /
// writeHwToThreads / syncHwBreaks / setRegister と、stop* / singleStepping_ ほかの状態
// （設計書 §4.2）。分けただけでアルゴリズムは変えていない。
//
// スレッドと同期（v1 のまま。§7.2-4）:
//   - onException は例外サーバスレッドで動く。停止を SessionSink::onRawStop で上位へ知らせ、
//     resumeCv_ で「resume 決定」を待つ。決定は上位がこのクラスの cont / stepInto / stepOver を
//     呼ぶことで与える（= decide）。決定を受けた例外スレッドが applyResume で対象を再開する。
//   - 手動停止（interrupt = Ctrl+C、dtrace stop() の取り込み、エントリ不明時の launch）は例外スレッドが
//     待っていないので、cont 等を呼んだスレッド（v2 ではエンジンスレッド）が applyResume を実行する。
//   - 停止状態（stopPc など）は「停止の通知後〜再開を決めるまで」の間だけ、決定者のスレッドから
//     読んでよい（v1 と同じ取り決め。個々のフィールドは mutex で守っていない）。
//
// L2 との境界: このクラスは「sink を呼ぶ / 決定を待つ」まで。積む先（EngineInbox）、停止の後処理
// （StopProcessor: イメージ更新・auto 継続・dlopen 内部停止の即 cont）、決定者（Debugger の
// コマンド処理）は L2 が持つ。
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <mach/mach.h>

#include "core/Breakpoints.h"
#include "core/DyldNotifier.h"
#include "core/SessionSink.h"
#include "platform/ExceptionServer.h"
#include "platform/Target.h"

namespace ta2 {

class Stepper {
public:
  Stepper(Target& target, Breakpoints& bps, ExceptionServer& es, DyldNotifier& dyld,
          SessionSink& sink)
    : target_(target), bps_(bps), es_(es), dyld_(dyld), sink_(sink) {}

  Stepper(const Stepper&) = delete;
  Stepper& operator=(const Stepper&) = delete;

  // ---- ライフサイクル（TargetSession が呼ぶ） ----
  // attach / launch 成功直後: スレッド状態バッファを Arch の大きさで確保する。
  void prepare();
  // launch: メイン実行ファイルのエントリ点 entry に一回限りの一時ブレイクを置き、
  // START_SUSPENDED 分を解除して走らせる（dyld 完了後に entry で onException が止める）。
  void armEntryStop(uint64_t entry);
  // launch でエントリ不明（LC_UNIXTHREAD のみ等）のとき: 現在地（dyld エントリ。タスクは
  // START_SUSPENDED のまま）を手動停止として確定し、sink へ知らせる。
  void enterInitialStop();
  // detach: 停止の種類に応じて全ブレイクを外し、対象を再開できる状態で手放す。
  //   手動停止中 → その場で後片付けして resume / 例外停止中 → 例外スレッドへ Detach を渡して
  //   完了（ack）を待つ / 実行中 → 一瞬 suspend して全ブレイクを外す。
  // この後 TargetSession が ExceptionServer::uninstall → Target::detach を行う。
  void releaseForDetach();
  // 例外サーバスレッドから呼ばれる（ExceptionServer::Handler の実体）。
  kern_return_t onException(thread_act_t thread, exception_type_t exc,
                            const int64_t* codes, int codeCount);

  // ---- 実行制御（決定者 = 上位のスレッドから呼ぶ） ----
  // 実行中に呼ぶ（Ctrl+C 相当）。対象を凍結して停止状態にし、sink へ知らせる。
  // 例外で止まったのではないので、再開は例外スレッドではなく cont 等を呼んだ側で行う。
  void interrupt();
  // 停止中に呼ぶ（対象を再開する = resume 決定）。
  void cont();
  void stepInto();
  void stepOver();

  // 停止中にレジスタへ書き込む（スレッド状態を更新して対象へ反映）。停止中のみ・成功で true。
  bool setRegister(const std::string& name, uint64_t value);
  // HW ブレイクの変更後に呼ぶ。全スレッドのデバッグレジスタへ現在の HW ブレイク集合を反映する
  // （実行中なら一瞬 suspend する）。SW ブレイクはメモリ側なのでこの呼び出しは不要。
  void syncHwBreaks();

  // ---- 停止状態の参照（sink 通知後、決定者が使う） ----
  bool stopped() const { return stopped_.load(); }
  uint64_t stopPc() const { return stopPc_; }
  thread_act_t stopThread() const { return stopThread_; }
  const std::vector<uint8_t>& stopState() const { return stopState_; } // スレッド状態バッファ
  int lastWatchHit() const { return lastWatchId_; } // 直近の停止がウォッチ命中ならその id、でなければ -1
  // 直近の停止が例外（クラッシュ）由来ならその種別（EXC_BAD_ACCESS 等）、通常停止なら 0。
  exception_type_t stopException() const { return stopExc_; }
  int64_t stopExcSubcode() const { return stopCode0_; }                // code[0]（BAD_ACCESS なら kern_return）
  uint64_t stopFaultAddress() const { return (uint64_t)stopCode1_; }   // code[1]（BAD_ACCESS のフォルト先）
  // 直近の停止が dyld のイメージ通知（dlopen 等）由来か。true なら上位は画面に出さず
  // イメージ更新＋保留ブレイク装着をして即 cont する（ユーザには止まって見えない）。
  bool stopIsImageLoad() const { return stopIsImageLoad_; }
  // 直近の停止が syscall 停止キャッチポイント由来か、そのとき発火した名前。
  bool stopIsSyscallCatch() const { return stopIsSyscallCatch_; }
  const std::string& syscallCaught() const { return scStopCaught_; }
  // 上の一式をまとめた値（sink へ渡すものと同じ形）。
  RawStop rawStop() const;

  // ---- SyscallMonitor が使う（dtrace の stop() 停止の合流） ----
  // dtrace consumer スレッドから、発火（= カーネルが対象タスクを suspend 済み）のたびに呼ぶ。
  // その suspend を計上に取り込んで通常の手動停止へ合流させ、sink へ知らせる。すでに別理由で
  // 停止中・未アタッチなら、dtrace が乗せた 1 段だけ相殺して無視する。
  void onExternalSuspend(const std::string& caughtName);
  // syscall キャッチ停止の停止スレッドを t に選び直す（発火スレッドの確定）。停止中のみ有効。
  // タスクは凍結済みなので、後から選び直しても状態は一貫する。
  void reselectStopThread(thread_act_t t);

private:
  // resume 決定をセットして例外スレッドを起こす
  enum class Resume { None, Cont, Step, StepOver, Detach };
  void decide(Resume r);
  void resumeManual(Resume r); // 手動停止からの再開（呼んだスレッドで applyResume）
  void applyResume(Resume r, thread_act_t thread);

  // 全スレッドのデバッグレジスタへ HW ブレイク集合を書き込む（呼び出し側がタスク停止を保証する）
  void writeHwToThreads();
  // ウォッチ命中か判定し、鳴ったウォッチ（無ければ nullptr）を返す。DR6 の発火ビットはクリアする。
  const Breakpoint* detectWatchHit(thread_act_t thread);
  // そのスレッドの HW 発火状態（x86:DR6）をクリアする。
  void clearHwHitOn(thread_act_t thread);
  void setStep(thread_act_t t, bool on); // Arch に委譲（x86:TF / arm64:SS）
  void notifyStop();                     // sink_.onRawStop(rawStop())

  Target& target_;
  Breakpoints& bps_;
  ExceptionServer& es_;
  DyldNotifier& dyld_;
  SessionSink& sink_;

  std::mutex m_;
  std::condition_variable resumeCv_; // 例外スレッドが resume 決定を待つ
  std::condition_variable ackCv_;    // detach の完了を呼び出し側が待つ
  Resume decision_ = Resume::None;
  bool detachAck_ = false;

  std::atomic<bool> stopped_{false};
  bool manualStop_ = false; // 手動停止（例外スレッドは待機していない）
  thread_act_t stopThread_ = 0;
  uint64_t stopPc_ = 0;
  std::vector<uint8_t> stopState_;

  // ステップ状態（単一スレッド対象を前提）
  bool singleStepping_ = false; // いま TF/SS を立てて 1 命令実行中
  bool stopAfterStep_ = false;  // そのステップ後に停止するか（step=真, cont のまたぎ=偽）
  bool rearmValid_ = false;     // 次のトラップ後に張り直すソフトブレイク
  uint64_t rearmAddr_ = 0;
  bool tempValid_ = false;      // stepOver / エントリ停止の一時ブレイク
  uint64_t tempAddr_ = 0;
  std::set<int> hwRearmIds_;    // 次のトラップ後に反映を戻す HW ブレイク/ウォッチの id（またぎステップ中）
  int watchStepId_ = -1;        // arm64: ウォッチ命中後、再開時に 1 命令跨ぐべきウォッチの id（x86 は -1 のまま）
  int lastWatchId_ = -1;        // 直近の停止がウォッチ命中ならその id（表示用）
  exception_type_t stopExc_ = 0;// 直近の停止が例外由来ならその種別（0=通常のブレイク/ステップ停止）
  int64_t stopCode0_ = 0;       // 例外 code[0]（BAD_ACCESS なら kern_return の種別）
  int64_t stopCode1_ = 0;       // 例外 code[1]（BAD_ACCESS ならフォルトしたアドレス）
  bool stopIsSyscallCatch_ = false; // 直近の停止が syscall 停止キャッチポイント由来か
  bool stopIsImageLoad_ = false;    // 直近の停止が dyld イメージ通知由来か（dlopen 等の内部停止）
  std::string scStopCaught_;        // そのとき発火した syscall/トラップ名
};

} // namespace ta2
