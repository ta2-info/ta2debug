#pragma once

// デバッグ対象プロセス。task_for_pid でタスクポートを取り、メモリ R/W・スレッド列挙・
// レジスタ取得/設定を提供する。対象は停止させない（アタッチ後も動き続ける）。
// アーキ依存は Arch に委ねる（スレッド状態 flavor、レジスタ配置など）。
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <mach/mach.h>

#include "core/Arch.h"

namespace ta2 {

class Target {
public:
  Target() = default;
  ~Target();

  Target(const Target&) = delete;
  Target& operator=(const Target&) = delete;

  // task_for_pid でアタッチ（対象は止めない）。失敗時は err に理由。
  bool attach(pid_t pid, std::string& err);
  // バイナリを一時停止状態で起動してアタッチ（START_SUSPENDED）。走り出す前に task を取る。
  // argv は末尾 nullptr 終端。子の stdout/stderr は <path>.ta2debug.log へ向ける。
  bool launch(const std::string& path, char* const argv[], std::string& err);
  // 手放す（自分で掛けた suspend は解除。タスクポートを deallocate。起動した子なら終了させる）。
  void detach();

  bool launched() const { return launched_; } // 自分で起動した子か

  bool isAttached() const { return task_ != 0; }
  pid_t pid() const { return pid_; }
  task_t task() const { return task_; }
  Arch& arch() { return *arch_; }

  // タスク全体の一時停止/再開（ネスト対応）
  bool suspend();
  bool resume();

  // dtrace の stop() など「外部」が掛けた task サスペンドを自分の計上に取り込む。
  // task_suspend は呼ばない（既に外部が suspend 済み）。以後 resume() 1 回で解除される。
  void adoptExternalSuspend();
  // 既に自分が停止中に外部サスペンドが 1 段乗ったとき、その 1 段だけを相殺する
  // （計上は変えない＝取り込まない）。既存の停止/再開収支に影響させないために使う。
  void cancelExternalSuspend();

  // メモリ R/W。write はコードページでも書ける（CoW + 保護復元。設計書 §7）。
  bool readMemory(uint64_t addr, void* buf, size_t size, size_t* outRead = nullptr);
  bool writeMemory(uint64_t addr, const void* buf, size_t size);

  // スレッド一覧（mach の thread_act_t）
  std::vector<thread_act_t> threads();

  // スレッド状態（バッファは arch().threadStateCount()*sizeof(natural_t) バイト）
  bool getThreadState(thread_act_t t, void* buf);
  // ベクタ/浮動小数レジスタの状態を取得（out は適切なサイズへ確保される）。未対応アーキ・失敗で false。
  bool getFloatState(thread_act_t t, std::vector<uint8_t>& out);
  bool setThreadState(thread_act_t t, const void* buf);
  bool getDebugState(thread_act_t t, void* buf);
  bool setDebugState(thread_act_t t, const void* buf);

private:
  pid_t pid_ = 0;
  task_t task_ = 0;
  std::unique_ptr<Arch> arch_;
  std::atomic<int> suspendCount_{0}; // consumer スレッドからも触る（dtrace stop 取り込み）ため atomic
  bool launched_ = false; // 自分で起動した子（detach 時に終了させる）
};

} // namespace ta2
