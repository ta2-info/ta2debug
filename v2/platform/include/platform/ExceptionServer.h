#pragma once

// L0 Platform（ta2platform）: Mach 例外ポートの差し替えと受信。対象タスクの EXC_BREAKPOINT
// （と BAD_ACCESS / BAD_INSTRUCTION / ARITHMETIC）を自前のポートへ向け、専用スレッドで
// mach_msg_server を回して受信する。受けた例外は Handler に転送するだけ（上位層は知らない）。
// MIG 生成の mach_exc サーバ（mach_excServer.c）を使うのはこの .cpp だけ。
// v1（inc/core/ExceptionServer.h）との差: install のエラーを Msg& へ（設計書 §8.1）。
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include <mach/mach.h>

#include "base/Msg.h"

namespace ta2 {

class ExceptionServer {
public:
  // thread/exception と code 配列を受け取り、処理結果（KERN_SUCCESS で対象を再開）を返す。
  using Handler = std::function<kern_return_t(thread_act_t thread,
                                              exception_type_t exception,
                                              const int64_t* codes,
                                              int codeCount)>;

  ExceptionServer() = default;
  ~ExceptionServer();

  ExceptionServer(const ExceptionServer&) = delete;
  ExceptionServer& operator=(const ExceptionServer&) = delete;

  bool install(task_t task, Handler handler, Msg& err);
  void uninstall();

  // 差し替え前の例外ポートへ戻す（クラッシュ継続時に、以降の例外を本来のハンドラへ委譲する用）。
  // 以後この対象の例外はこちらへは来ない。uninstall も内部でこれを使う。
  void restoreOriginalPorts();

  // MIG の catch 関数から呼ばれる内部ディスパッチ（public だが外部からは使わない）
  kern_return_t dispatch(thread_act_t thread, exception_type_t exception,
                         const int64_t* codes, int codeCount);

private:
  task_t task_ = 0;
  mach_port_t port_ = MACH_PORT_NULL;
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  // 差し替え前の例外ポート（復元用）
  exception_mask_t savedMasks_[32];
  mach_port_t savedPorts_[32];
  exception_behavior_t savedBehaviors_[32];
  thread_state_flavor_t savedFlavors_[32];
  mach_msg_type_number_t savedCount_ = 0;
};

} // namespace ta2
