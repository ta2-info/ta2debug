#pragma once

// L1 Core（ta2core）: 1 対象ぶんの部品の所有者。由来は v1 Session から Stepper / DyldNotifier /
// SyscallMonitor / SymbolWarmer / ExitMonitor / SyscallDecoder を切り出した「残り」
// （attach / launch / detach、findEntryPoint、describe、symbolName、sourceLineFor、defaultLib。
// 設計書 §4.2）。Target / Breakpoints / ImageList / Symbols / Annotations と上記の部品を束ね、
// 生成・破棄の順序（v1 Session::detach の順序）を守る。
//
// 上位（L2）とのつながりは SessionSink だけ（v1 は app/EventQueue を直接握っていた）:
//   - 停止は Stepper が sink.onRawStop で知らせ、上位は stepper().cont() 等で再開を決める。
//   - 終了は ExitMonitor → sink.onTargetExited、先読みの進捗は SymbolWarmer → sink.onWarmProgress。
// 「停止のたびに行う後処理」（refreshImages / refreshAnnotations / syscalls().refineStopThread /
// warmSymbols / dyld().ensure、dlopen 内部停止の即 cont、auto 継続）は呼び出し順ごと L2 の
// StopProcessor が持つ。ここはその材料を公開するところまで。
//
// 式評価の文脈（EvalContext）もここが実装する（v1 の Expression は Session& を直接見ていた）。
#include <cstdint>
#include <string>
#include <unordered_map>

#include "base/Msg.h"
#include "core/Annotations.h"
#include "core/Breakpoints.h"
#include "core/DwarfLines.h"
#include "core/DyldNotifier.h"
#include "core/ExitMonitor.h"
#include "core/Expression.h"
#include "core/ImageList.h"
#include "core/SessionSink.h"
#include "core/Stepper.h"
#include "core/SymbolRef.h"
#include "core/SymbolWarmer.h"
#include "core/Symbols.h"
#include "core/SyscallDecoder.h"
#include "core/SyscallMonitor.h"
#include "platform/ExceptionServer.h"
#include "platform/Target.h"

namespace ta2 {

class TargetSession : public EvalContext {
public:
  // sink はこのオブジェクトより長生きすること。
  explicit TargetSession(SessionSink& sink);
  ~TargetSession() override;

  TargetSession(const TargetSession&) = delete;
  TargetSession& operator=(const TargetSession&) = delete;

  // ---- 対象の管理 ----
  bool attach(pid_t pid, Msg& err);
  // バイナリを起動してデバッグ開始（START_SUSPENDED で起動し、メイン実行ファイルのエントリ点で
  // 止める。エントリ不明なら dyld エントリで止める）。停止は sink.onRawStop で届く。
  bool launch(const std::string& path, char* const argv[], Msg& err);
  // 手放す。順序は v1 のまま: dtrace 停止 → 先読み停止 → 終了監視停止 → Stepper の後片付け
  // （例外スレッドへ Detach を渡して ack を待つ等）→ 例外ポート復元 → Target::detach。
  void detach();
  bool isAttached() { return target_.isAttached(); }
  pid_t pid() { return target_.pid(); }

  // ---- 部品 ----
  Target& target() { return target_; }
  Arch& arch() { return target_.arch(); }
  Breakpoints& breaks() { return bps_; }
  ImageList& images() { return images_; }
  Symbols& symbols() { return symbols_; }
  Annotations& annotations() { return annots_; }
  Stepper& stepper() { return stepper_; }
  DyldNotifier& dyld() { return dyld_; }
  SyscallMonitor& syscalls() { return scmon_; }
  SymbolWarmer& warmer() { return warmer_; }

  // HW ブレイクの変更後に呼ぶ（Stepper::syncHwBreaks へ委譲）。
  void syncHwBreaks() { stepper_.syncHwBreaks(); }
  // 表示用: ブレイク命令バイトを元命令に見せる
  void maskBreakpoints(uint64_t base, uint8_t* buf, size_t len) { bps_.maskInto(base, buf, len); }

  // ユーザ注釈ファイル（UUID 別）を、更新されていれば読み直す。重い探索を別スレッドで走らせる前と、
  // 停止時など状態を変えるスレッドの境界で呼ぶ。読み込み済み全イメージを安価に stat するだけ。
  void refreshAnnotations() { for (const auto& im : images_.images()) annots_.refresh(im); }
  // 停止時などに一覧が変わっていれば読み直す（dlopen 追従）。読み直したら true。
  bool refreshImages() { return images_.refreshIfChanged(); }

  // 式でライブラリ名を省略したときに最初に探す既定ライブラリ（uselib）。
  void setDefaultLib(const std::string& name) { defaultLib_ = name; }
  const std::string& defaultLib() const { return defaultLib_; }
  // 既定ライブラリの Image（未設定／見つからなければ主実行ファイル）。
  const Image* defaultLibImage();

  // アドレスの構造化記述（イメージ + オフセット + シンボル + メモ）。v1 の describe 文字列は
  // formatDescribe(describe(addr))、symbolName 文字列は formatSymbolName(...) で得られる。
  SymbolRef describe(uint64_t addr);
  // 関数名だけを短く（"func" / "func+0xNN" / "name [stub]"。無ければイメージ名）。call 先注記用。
  std::string symbolName(uint64_t addr);

  // 停止位置が syscall/svc 命令なら、これから呼ぶシステムコールを解析して out に入れ true。
  // 停止中でない・該当しないなら false。
  bool syscallAtStop(SyscallCall& out);

  // 実行時アドレスのソース位置（dSYM / debug map の DWARF 行情報）。見つかれば true
  // （out.text にソース行も）。
  bool sourceLineFor(uint64_t addr, SourceLoc& out);

  // ロード済み全イメージのシンボルをバックグラウンドで先読み・キャッシュする（一度だけ起動。冪等）。
  // 無効化は warmer().setEnabled(false)。
  void warmSymbols();

  // ---- EvalContext（Expression 用） ----
  bool evalStopped() override { return stepper_.stopped(); }
  bool evalReadRegister(const std::string& loweredName, uint64_t& out) override;
  ImageList& evalImages() override { return images_; }
  Symbols& evalSymbols() override { return symbols_; }
  const Image* evalDefaultLibImage() override { return defaultLibImage(); }

private:
  // メイン実行ファイル（MH_EXECUTE）のエントリ点を実行時アドレスで返す（0=不明）。launch の初期停止用。
  uint64_t findEntryPoint();
  bool installExceptionHandler(Msg& err);
  DwarfLines* linesForImage(const Image& img);

  SessionSink& sink_;
  // メンバの並び = 生成順（後のものが前のものを参照する）。破棄は逆順だが、~TargetSession が
  // 先に detach() でスレッドを全部止めるので、破棄順に依存する処理は残らない。
  Target target_;
  Breakpoints bps_;
  ExceptionServer es_;
  ImageList images_;
  Symbols symbols_;
  Annotations annots_;   // ユーザ注釈（名前／メモ）。symbols_ に重ねる
  DyldNotifier dyld_;
  Stepper stepper_;
  SyscallMonitor scmon_;
  SymbolWarmer warmer_;
  ExitMonitor exitMon_;

  std::string defaultLib_; // uselib で設定する既定ライブラリ名（空＝主実行ファイル）
  // DWARF 行情報（イメージ UUID でキャッシュ、遅延ロード）。
  std::unordered_map<std::string, DwarfLines> lineCache_;
};

} // namespace ta2
