#pragma once

// libdtrace に相乗りして、対象プロセスのシステムコールを検知する。
// カーネルのシステムコール・ディスパッチ表を計測するので単一ステップ不要で、全 syscall を
// 低コストで捕捉できる（ユーザ空間デバッガ単体では不可能な領域）。SIP 無効＋root 前提。
// Mach-O/libdtrace/D はいずれもアーキ非依存なので Arch 依存なし。
//
// Phase 1（本ファイル）: 観測のみ（発火ごとに handler を呼ぶ）。
// Phase 2 で stop() アクションによる「その場停止」を足し、デバッガの停止へ合流させる予定。
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <thread>

namespace ta2 {

struct SyscallHit {
  std::string name;        // syscall 名（probe の func 名。例 "write" / "write_nocancel"）
  bool isReturn = false;   // entry/return
  // 引数は dtrace のレコード生データ（ABI 依存）には触れず、停止時に対象レジスタから
  // 取る方針（Session::syscallAtStop が既に解析済み）。ここでは名前と entry/return のみ。
};

class DtraceCatch {
public:
  using Handler = std::function<void(const SyscallHit&)>;
  ~DtraceCatch();

  // pid の syscall を監視開始。filter が空 or "all" で全件、"write" 等で特定 syscall。
  // 発火ごとに handler（バックグラウンドスレッドから呼ばれる）。成功で true、失敗は err に理由。
  bool start(pid_t pid, const std::string& filter, Handler handler, std::string& err);
  void stop();                 // 停止・解放（冪等）。
  bool active() const { return active_.load(); }

  // --- 内部: libdtrace の consume コールバックから呼ぶ（worker スレッド専用） ---
  void cbBegin(const char* func, const char* name);
  void cbEnd();

private:
  void loop();

  void* hdl_ = nullptr;        // dtrace_hdl_t*（dtrace.h を公開ヘッダに出さないため void*）
  std::thread worker_;
  std::atomic<bool> active_{false};
  std::atomic<bool> stopReq_{false};
  Handler handler_;
  SyscallHit pending_;         // 組み立て中の 1 件（worker スレッド専用）
};

} // namespace ta2
