#pragma once

// L1 Core（ta2core）: syscall / Mach トラップの監視（libdtrace 相乗り）。由来は v1 Session の
// startSyscallWatch / startSyscallStop / stopSyscallWatch / onSyscallStopHit /
// refineSyscallStopThread / firingThreadFor と sc* 状態（設計書 §4.2）。
//   :scwatch … 観測（全件は名前別の集計、名指しは引数・戻り値つきトレース）
//   :scstop  … 停止キャッチポイント（dtrace の stop() でカーネルが対象を凍結 → 通常停止へ合流）
// 両者は同じ dtrace ハンドルを使うので排他（片方を始めるともう片方は解除される）。
// 停止への合流そのもの（suspend の取り込み・停止状態の確定・通知）は Stepper が行う。
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/Msg.h"
#include "core/Stepper.h"
#include "platform/DtraceCatch.h"
#include "platform/Target.h"

namespace ta2 {

class SyscallMonitor {
public:
  // アドレス → "func" / "func+0xNN"（v1 Session::symbolName と同じ書式）。発火スレッドの確定で
  // 各スレッドの PC シンボルを捕捉名と突き合わせるのに使う。TargetSession が渡す。
  using SymbolNamer = std::function<std::string(uint64_t addr)>;

  SyscallMonitor(Target& target, Stepper& stepper, SymbolNamer namer)
    : target_(target), stepper_(stepper), namer_(std::move(namer)) {}
  ~SyscallMonitor() { stop(); }

  SyscallMonitor(const SyscallMonitor&) = delete;
  SyscallMonitor& operator=(const SyscallMonitor&) = delete;

  // ---- 観測モード ----
  // filter 空/"all" で全 syscall、"write" 等で特定。カーネル計測なので単一ステップ不要。
  // 全件は発火ごとに名前別カウントを貯める（高頻度でも UI を溢れさせない集計方式）。名指しは
  // 引数・戻り値つきでトレースし、直近の呼び出しを保持する。要 attach（err.session.not_attached）。
  bool startWatch(const std::string& filter, Msg& err);
  // ---- 停止キャッチポイント（libdtrace の stop() アクション） ----
  // filter は観測版と同じ書式（""/"all"/"<名前>"/"mach"/"mach:<名前>"）。マッチした
  // syscall/Mach トラップが発火した瞬間にカーネルが対象タスクを凍結し、通常の停止として扱う
  // （レジスタ/バックトレース/メモリを検査でき、cont で次の発火まで走る）。要 attach。
  bool startStop(const std::string& filter, Msg& err);
  // 監視をやめる（観測・停止キャッチポイントのどちらでも。冪等）。consumer スレッドを止めるので、
  // Target の破棄前に必ず呼ぶこと。
  void stop();

  bool active() const { return dtrace_.active(); }
  // 現在の dtrace 稼働が停止キャッチポイントか（観測モードなら false）。
  bool stopMode() const { return scStop_.load(); }
  // 特定 syscall/トラップを名指し監視したときは引数・戻り値つきでトレースする（全件は集計のみ）。
  bool traceMode() const { return scTraceMode_.load(); }
  std::string filter();                 // 監視対象（表示用。空なら "all"）
  uint64_t total() const { return scTotal_.load(); }
  // 名前→回数（表示用コピー）。回数の多い順に使うのは呼び出し側。
  std::vector<std::pair<std::string, uint64_t>> counts();
  // 直近のトレース行（"write(0x.., 3, 3) = 3 errno=0" など）を古い順でコピー。新しい順は呼び出し側で。
  std::vector<std::string> trace();

  // 停止キャッチポイント停止後に、決定者のスレッド（v1 のメイン、v2 のエンジン）から呼ぶ。
  // 捕捉した syscall/トラップ名と各スレッドの PC シンボルを突き合わせ、発火スレッドへ停止スレッドを
  // 確定する（複数スレッド対象で正しい BT）。シンボル解決を伴うため consumer スレッドではなく
  // ここで行う。停止中のみ有効。
  void refineStopThread();

  // 監視に使える syscall / Mach トラップ名の一覧（attach 不要・監視中でも可）。DtraceCatch へ委譲。
  static bool listNames(const std::string& filter, std::vector<std::string>& bsd,
                        std::vector<std::string>& mach, Msg& err) {
    return DtraceCatch::listNames(filter, bsd, mach, err);
  }

private:
  // 捕捉名（"write" / "mach:<名前>"）に対し、その syscall/トラップを呼んでいるスレッドを
  // PC シンボルで探す（無ければ 0）。停止中・タスク凍結済みで呼ぶ。
  thread_act_t firingThreadFor(const std::string& caught);

  Target& target_;
  Stepper& stepper_;
  SymbolNamer namer_;
  DtraceCatch dtrace_; // syscall 監視（libdtrace 相乗り）
  std::mutex scMu_;    // scCounts_/scTrace_/scFilter_ を保護（consumer スレッドと共用）
  std::unordered_map<std::string, uint64_t> scCounts_; // syscall 名→回数（全件集計モード）
  std::deque<std::string> scTrace_;                    // 直近のトレース行（名指しトレースモード）
  std::atomic<uint64_t> scTotal_{0};                   // 総発火数
  std::string scFilter_;                               // 監視対象（表示用）
  std::atomic<bool> scStop_{false};                    // 現在の dtrace 稼働が停止キャッチポイントか
  std::atomic<bool> scTraceMode_{false};               // 観測が引数つきトレースモードか（全件は集計）
};

} // namespace ta2
