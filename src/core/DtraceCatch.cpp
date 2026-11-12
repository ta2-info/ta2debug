#include "core/DtraceCatch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dtrace.h>

// TA2_DT_DEBUG=1 で consumer の動作を stderr に出す（開発時の確認用）。
static const bool g_dtDbg = getenv("TA2_DT_DEBUG") != nullptr;

namespace ta2 {

// libdtrace の consume コールバック（ファイル内 static）。arg に DtraceCatch* を渡す。
// probe 発火ごとに s_probe → 各レコード s_rec → 末尾に rec==NULL の s_rec が来る（dtrace(1) と同じ約束）。
namespace {
int s_probe(const dtrace_probedata_t* d, void* arg) {
  auto* self = static_cast<DtraceCatch*>(arg);
  const dtrace_probedesc_t* pd = d->dtpda_pdesc;
  self->cbBegin(pd ? pd->dtpd_provider : "", pd ? pd->dtpd_func : "", pd ? pd->dtpd_name : "");
  return DTRACE_CONSUME_THIS;
}
int s_rec(const dtrace_probedata_t* d, const dtrace_recdesc_t* rec, void* arg) {
  (void)d;
  auto* self = static_cast<DtraceCatch*>(arg);
  if (rec == nullptr) { self->cbEnd(); return DTRACE_CONSUME_NEXT; } // この probe のレコード終端
  // レコードの生データ（libdtrace 内部バッファ・ABI 依存）には触れない。名前は probe 記述から得る。
  // 発火スレッドの特定は停止後に対象スレッドの PC シンボルで行う（Session 側）。
  return DTRACE_CONSUME_THIS;
}
} // namespace

void DtraceCatch::cbBegin(const char* provider, const char* func, const char* name) {
  pending_ = SyscallHit{};
  // Mach トラップは BSD syscall と区別できるよう "mach:" を前置する。
  bool mach = (provider && std::strcmp(provider, "mach_trap") == 0);
  pending_.name = (mach ? std::string("mach:") : std::string()) + (func ? func : "");
  pending_.isReturn = (name && std::strcmp(name, "return") == 0);
}
void DtraceCatch::cbEnd() {
  if (g_dtDbg) fprintf(stderr, "[dt] hit %s%s\n", pending_.name.c_str(), pending_.isReturn ? " (return)" : "");
  if (handler_) handler_(pending_);
}

bool DtraceCatch::start(pid_t pid, const std::string& filter, Handler handler, std::string& err,
                        bool stopOnHit) {
  if (hdl_) { err = "既に監視中"; return false; }
  handler_ = std::move(handler);
  stopReq_ = false;

  // filter を provider と名前に分解する。
  //   ""/"all" → syscall 全件 / "<名前>" → syscall::<名前>
  //   "mach"/"mach:" → mach_trap 全件 / "mach:<名前>" → mach_trap::<名前>
  std::string provider = "syscall", name = filter;
  if (filter == "mach" || filter.rfind("mach:", 0) == 0) {
    provider = "mach_trap";
    name = (filter.size() > 5) ? filter.substr(5) : "";
  }
  if (name == "all") name = "";
  // trace(0) は「発火を consumer に届かせる」ための最小レコード（値は読まない）。
  // /pid==対象/ でそのプロセスだけに絞る。
  // stopOnHit のときは破壊的アクション stop() を足す。カーネルが発火時に対象タスクを
  // suspend する（精密＝syscall 境界で止まる）。destructive は pragma で有効化する
  // （dtrace(1) の -w 相当。これが無いと stop() はコンパイルを通らない）。
  std::string prog;
  if (stopOnHit) prog += "#pragma D option destructive\n";
  prog += provider + "::" + name + ":entry /pid == " + std::to_string((long long)pid) + "/ { trace(0);";
  if (stopOnHit) prog += " stop();";
  prog += " }";

  int derr = 0;
  dtrace_hdl_t* h = dtrace_open(DTRACE_VERSION, 0, &derr);
  if (!h) { err = std::string("dtrace_open 失敗: ") + dtrace_errmsg(nullptr, derr); return false; }

  // バッファと、停止要求に素早く反応するための切替レート。
  dtrace_setopt(h, "bufsize", "4m");
  dtrace_setopt(h, "switchrate", "10hz");

  dtrace_prog_t* p = dtrace_program_strcompile(h, prog.c_str(), DTRACE_PROBESPEC_NAME,
                                               DTRACE_C_ZDEFS, 0, nullptr);
  if (!p) {
    err = std::string("D コンパイル失敗: ") + dtrace_errmsg(h, dtrace_errno(h));
    dtrace_close(h);
    return false;
  }
  dtrace_proginfo_t info;
  if (dtrace_program_exec(h, p, &info) == -1) {
    err = std::string("program_exec 失敗: ") + dtrace_errmsg(h, dtrace_errno(h));
    dtrace_close(h);
    return false;
  }
  if (dtrace_go(h) == -1) {
    err = std::string("dtrace_go 失敗: ") + dtrace_errmsg(h, dtrace_errno(h));
    dtrace_close(h);
    return false;
  }

  hdl_ = h;
  active_ = true;
  if (g_dtDbg) fprintf(stderr, "[dt] go ok. prog=%s\n", prog.c_str());
  worker_ = std::thread([this] { loop(); });
  return true;
}

void DtraceCatch::loop() {
  auto* h = static_cast<dtrace_hdl_t*>(hdl_);
  // dtrace_work の既定整形出力の捨て先（レコードは callback で消費するので通常使われない。
  // ただし fp=NULL だと内部でエラーになるため有効な FILE* を渡す）。
  FILE* devnull = fopen("/dev/null", "w");
  while (!stopReq_.load()) {
    dtrace_sleep(h); // switchrate 間隔で起きる（stop 要求に ~100ms で反応）
    dtrace_workstatus_t st = dtrace_work(h, devnull, s_probe, s_rec, this);
    if (g_dtDbg && st != DTRACE_WORKSTATUS_OKAY)
      fprintf(stderr, "[dt] work status=%d err=%s\n", (int)st,
              st == DTRACE_WORKSTATUS_ERROR ? dtrace_errmsg(h, dtrace_errno(h)) : "");
    if (st == DTRACE_WORKSTATUS_DONE || st == DTRACE_WORKSTATUS_ERROR) break;
  }
  if (devnull) fclose(devnull);
  active_ = false;
}

void DtraceCatch::stop() {
  if (!hdl_) return;
  stopReq_ = true;
  if (worker_.joinable()) worker_.join();
  auto* h = static_cast<dtrace_hdl_t*>(hdl_);
  dtrace_stop(h);
  dtrace_close(h);
  hdl_ = nullptr;
  active_ = false;
}

DtraceCatch::~DtraceCatch() { stop(); }

} // namespace ta2
