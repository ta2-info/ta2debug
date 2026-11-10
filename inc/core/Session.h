#pragma once

// デバッグセッションの芯。Target + Breakpoints + ExceptionServer をまとめ、
// 「止まる／続ける／ステップ」の状態機械（Stepper 相当）を持つ。
// 例外スレッドで停止を検知したらメインへ Stopped イベントを送り、メインの resume 操作
// （cont/stepInto/stepOver/detach）を待って対象を再開する。対象は普段は動かし続ける。
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/Target.h"
#include "core/Breakpoints.h"
#include "core/ExceptionServer.h"
#include "core/ImageList.h"
#include "core/Symbols.h"
#include "core/Annotations.h"
#include "core/DwarfLines.h"
#include "app/EventQueue.h"
#include <unordered_map>

namespace ta2 {

class Session {
public:
  explicit Session(EventQueue& queue)
    : queue_(queue), bps_(target_), images_(target_), symbols_(target_) {
    symbols_.setAnnotations(&annots_); // ユーザ注釈をシンボル解決に重ねる
  }
  ~Session();

  bool attach(pid_t pid, std::string& err);
  // バイナリを起動してデバッグ開始（START_SUSPENDED でエントリ手前で停止状態にする）。
  bool launch(const std::string& path, char* const argv[], std::string& err);
  void detach();
  bool isAttached() { return target_.isAttached(); }
  pid_t pid() { return target_.pid(); }

  // ブレイクポイント（ユーザ操作は Breakpoints 経由。アタッチ中のみ有効）
  Breakpoints& breaks() { return bps_; }
  // HW ブレイクの変更後に呼ぶ。全スレッドのデバッグレジスタへ現在の HW ブレイク集合を反映する
  // （実行中なら一瞬 suspend する）。SW ブレイクはメモリ側なのでこの呼び出しは不要。
  void syncHwBreaks();
  // 表示用: ブレイク命令バイトを元命令に見せる
  void maskBreakpoints(uint64_t base, uint8_t* buf, size_t len) { bps_.maskInto(base, buf, len); }

  // 実行中にメインから呼ぶ（Ctrl+C 相当）。対象を凍結して停止状態にし、Stopped を通知する。
  // 例外で止まったのではないので、再開は resume 決定を待つ例外スレッドではなくこちら側で行う。
  void interrupt();

  // 停止中にメインから呼ぶ（対象を再開する）
  void cont();
  void stepInto();
  void stepOver();

  // 停止中にレジスタへ書き込む（スレッド状態を更新して対象へ反映）。停止中のみ・成功で true。
  bool setRegister(const std::string& name, uint64_t value);

  // 停止状態の参照（Stopped イベント後、メインが描画に使う）
  bool stopped() { return stopped_.load(); }
  uint64_t stopPc() { return stopPc_; }
  int lastWatchHit() const { return lastWatchId_; } // 直近の停止がウォッチ命中ならその id、でなければ -1
  // 直近の停止が例外（クラッシュ）由来ならその種別（EXC_BAD_ACCESS 等）、通常停止なら 0。
  exception_type_t stopException() const { return stopExc_; }
  int64_t stopExcSubcode() const { return stopCode0_; }  // code[0]（BAD_ACCESS なら kern_return）
  uint64_t stopFaultAddress() const { return (uint64_t)stopCode1_; } // code[1]（BAD_ACCESS のフォルト先）
  // 停止理由を人間向けに整形。通常停止なら空文字。例: "EXC_BAD_ACCESS (KERN_INVALID_ADDRESS) @0x..."
  std::string stopReason() const;
  thread_act_t stopThread() { return stopThread_; }
  const std::vector<uint8_t>& stopState() { return stopState_; } // スレッド状態バッファ

  Target& target() { return target_; }
  Arch& arch() { return target_.arch(); }
  ImageList& images() { return images_; }
  Symbols& symbols() { return symbols_; }
  Annotations& annotations() { return annots_; }

  // ユーザ注釈ファイル（UUID 別）を、更新されていれば読み直す（条件2）。
  // 重い探索を別スレッドで走らせる前と、停止時などメインスレッドの境界で呼ぶ。
  // 読み込み済み全イメージを対象に安価に stat するだけ（変化が無ければ何もしない）。
  void refreshAnnotations() { for (const auto& im : images_.images()) annots_.refresh(im); }

  // 式でライブラリ名を省略したときに最初に探す既定ライブラリ（uselib）。
  void setDefaultLib(const std::string& name) { defaultLib_ = name; }
  const std::string& defaultLib() const { return defaultLib_; }
  // 既定ライブラリの Image（未設定／見つからなければ主実行ファイル）。
  const Image* defaultLibImage();
  // 停止時などに一覧が変わっていれば読み直す（dlopen 追従）
  void refreshImages() { images_.refreshIfChanged(); }
  // アドレスを "image 0x.. func+0xNN"（無ければ "image 0x.." / 空）に整形（ステータス用）
  std::string describe(uint64_t addr);
  // 関数名だけを短く（"func" / "func+0xNN"）。無ければイメージ名。call 先注記用。
  std::string symbolName(uint64_t addr);
  // 停止位置が syscall/svc 命令なら、これから呼ぶシステムコールを解析して
  // "write(1, 0x.., 14) \"hi\"" のように整形。ポインタ引数は参照先も表示。該当しなければ空。
  std::string syscallAtStop();

  // 実行時アドレスのソース位置（dSYM の DWARF 行情報）。見つかれば true（out.text にソース行も）。
  bool sourceLineFor(uint64_t addr, SourceLoc& out);

  // ロード済み全イメージのシンボルをバックグラウンドで先読み・キャッシュする（一度だけ起動）。
  // UUID キャッシュ（Symbols）へ保存されるので次回以降は速い。detach 時に安全に停止する。
  void warmSymbols();
  // 先読みの有効/無効（バッチは一度きりなので無効化してオーバーヘッドを無くす）。
  void setWarmingEnabled(bool on) { warmingEnabled_ = on; }
  // シンボル先読みの進捗（表示用）。warming 中か / 済んだ数 / 総数。
  bool symbolsWarming() const { return warmActive_.load(); }
  int symbolsWarmedDone() const { return warmDone_.load(); }
  int symbolsWarmedTotal() const { return warmTotal_.load(); }

private:
  // 例外スレッドから呼ばれる
  kern_return_t onException(thread_act_t thread, exception_type_t exc,
                            const int64_t* codes, int codeCount);

  // resume 決定をセットして例外スレッドを起こす
  enum class Resume { None, Cont, Step, StepOver, Detach };
  void decide(Resume r);
  void resumeManual(Resume r); // Ctrl+C 停止からの再開（こちらのスレッドで applyResume）

  EventQueue& queue_;
  Target target_;
  Breakpoints bps_;
  ExceptionServer es_;
  ImageList images_;
  Symbols symbols_;
  Annotations annots_; // ユーザ注釈（名前／メモ）。symbols_ に重ねる
  std::string defaultLib_; // uselib で設定する既定ライブラリ名（空＝主実行ファイル）
  // dSYM の DWARF 行情報（イメージ UUID でキャッシュ、遅延ロード）。
  std::unordered_map<std::string, DwarfLines> lineCache_;
  DwarfLines* linesForImage(const Image& img);

  std::mutex m_;
  std::condition_variable resumeCv_; // 例外スレッドが resume 決定を待つ
  std::condition_variable ackCv_;    // detach の完了をメインが待つ
  Resume decision_ = Resume::None;
  bool detachAck_ = false;

  std::atomic<bool> stopped_{false};
  bool manualStop_ = false; // Ctrl+C による停止（例外スレッドは待機していない）
  thread_act_t stopThread_ = 0;
  uint64_t stopPc_ = 0;
  std::vector<uint8_t> stopState_;

  // Stepper 状態（単一スレッド対象を前提）
  bool singleStepping_ = false; // いま TF/SS を立てて 1 命令実行中
  bool stopAfterStep_ = false;  // そのステップ後に停止するか（step=真, cont のまたぎ=偽）
  bool rearmValid_ = false;     // 次のトラップ後に張り直すソフトブレイク
  uint64_t rearmAddr_ = 0;
  bool tempValid_ = false;      // stepOver の一時ブレイク
  uint64_t tempAddr_ = 0;
  std::set<int> hwRearmIds_;    // 次のトラップ後に反映を戻す HW ブレイク/ウォッチの id（またぎステップ中）
  int watchStepId_ = -1;        // arm64: ウォッチ命中後、再開時に 1 命令跨ぐべきウォッチの id（x86 は -1 のまま）
  int lastWatchId_ = -1;        // 直近の停止がウォッチ命中ならその id（表示用）
  exception_type_t stopExc_ = 0;// 直近の停止が例外由来ならその種別（0=通常のブレイク/ステップ停止）
  int64_t stopCode0_ = 0;       // 例外 code[0]（BAD_ACCESS なら kern_return の種別）
  int64_t stopCode1_ = 0;       // 例外 code[1]（BAD_ACCESS ならフォルトしたアドレス）

  // 全スレッドのデバッグレジスタへ HW ブレイク集合を書き込む（呼び出し側がタスク停止を保証する）
  void writeHwToThreads();
  // メイン実行ファイル（MH_EXECUTE）のエントリ点を実行時アドレスで返す（0=不明）。:run の初期停止用。
  uint64_t findEntryPoint();
  // ウォッチ命中か判定し、鳴ったウォッチ（無ければ nullptr）を返す。DR6 の発火ビットはクリアする。
  const Breakpoint* detectWatchHit(thread_act_t thread);
  // そのスレッドの HW 発火状態（x86:DR6）をクリアする。
  void clearHwHitOn(thread_act_t thread);
  void setStep(thread_act_t t, bool on); // Arch に委譲（x86:TF / arm64:SS）
  void applyResume(Resume r, thread_act_t thread);

  // 対象プロセスの終了監視（kqueue EVFILT_PROC/NOTE_EXIT）。終了したら TargetExited を通知。
  void startExitMonitor();
  void stopExitMonitor();
  std::thread monitor_;
  int monitorKq_ = -1;

  // シンボル先読み（warming）用のバックグラウンドスレッド。target 破棄前に必ず join する。
  void stopSymbolWarming();
  std::thread warmer_;
  std::atomic<bool> warmStop_{false};
  std::atomic<bool> warmStarted_{false};
  std::atomic<bool> warmingEnabled_{true}; // 先読みを行うか（バッチでは false）
  std::atomic<bool> warmActive_{false}; // 先読みスレッドが走行中か（表示用）
  std::atomic<int> warmDone_{0};         // 先読み済みイメージ数
  std::atomic<int> warmTotal_{0};        // 先読み対象の総数
};

} // namespace ta2
