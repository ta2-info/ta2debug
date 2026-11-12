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
  // 停止キャッチポイントの発火スレッドは停止後に PC シンボルで特定する（Session 側）。
};

class DtraceCatch {
public:
  using Handler = std::function<void(const SyscallHit&)>;
  using LineHandler = std::function<void(const std::string&)>; // 整形済みトレース 1 行
  ~DtraceCatch();

  // filter（start と同じ書式。ただし全件 "" / "all" / "mach" は不可＝名前必須）の syscall/
  // トラップを「引数・戻り値つき」で観測する。entry で引数を thread-local に退避し、return で
  // `名前(0x.., 0x.., ..) = 戻り値 errno=N` を dtrace の printf で整形、dtrace_handle_buffered
  // で整形済みテキストを受け取って 1 行ごとに onLine を呼ぶ（生レコードは読まない＝ABI 非依存）。
  // 全件モードが無いのは、全 syscall を引数つきで出すと洪水になるため（全件は start の集計を使う）。
  bool startTrace(pid_t pid, const std::string& filter, LineHandler onLine, std::string& err);

  // pid の syscall/Mach トラップを監視開始。filter の書式:
  //   ""/"all"      → 全 BSD syscall（syscall:::entry）
  //   "<名前>"       → その BSD syscall（syscall::<名前>:entry）
  //   "mach"         → 全 Mach トラップ（mach_trap:::entry）
  //   "mach:<名前>"  → その Mach トラップ（mach_trap::<名前>:entry）
  // Mach トラップの通知名は "mach:" を前置して BSD と区別する。
  // 発火ごとに handler（バックグラウンドスレッドから呼ばれる）。成功で true、失敗は err に理由。
  //
  // stopOnHit=true（Phase 2 の停止キャッチポイント）のときは D プログラムに破壊的アクション
  // stop() を加える（`#pragma D option destructive`）。発火した瞬間にカーネルが対象タスクを
  // suspend する（task レベル。再開は呼び出し側が task_resume で行う）。handler は従来どおり
  // 1 発ごとに呼ばれるが、stop 中はタスクが凍結するので次の再開まで追加の発火は無い。
  bool start(pid_t pid, const std::string& filter, Handler handler, std::string& err,
             bool stopOnHit = false);
  void stop();                 // 停止・解放（冪等）。
  bool active() const { return active_.load(); }

  // --- 内部: libdtrace の consume コールバックから呼ぶ（worker スレッド専用） ---
  void cbBegin(const char* provider, const char* func, const char* name);
  void cbEnd();
  // dtrace_handle_buffered から呼ぶ。整形済みの断片を貯め、改行ごとに onLine_ を呼ぶ。
  void cbBuffered(const char* text);

  // filter を provider と名前に分解（start/startTrace で共用）。
  static void parseFilter(const std::string& filter, std::string& provider, std::string& name);

private:
  void loop();

  void* hdl_ = nullptr;        // dtrace_hdl_t*（dtrace.h を公開ヘッダに出さないため void*）
  std::thread worker_;
  std::atomic<bool> active_{false};
  std::atomic<bool> stopReq_{false};
  Handler handler_;
  SyscallHit pending_;         // 組み立て中の 1 件（worker スレッド専用）
  LineHandler onLine_;         // トレース行の受け手（startTrace のとき）
  std::string lineAccum_;      // 整形出力の未完行バッファ（worker スレッド専用）
};

} // namespace ta2
