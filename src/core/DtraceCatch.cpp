#include "core/DtraceCatch.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
void DtraceCatch::cbBuffered(const char* text) {
  // 整形出力は断片で届き得るので改行で 1 行に組み立てて onLine_ に渡す。
  lineAccum_ += text;
  size_t nl;
  while ((nl = lineAccum_.find('\n')) != std::string::npos) {
    std::string line = lineAccum_.substr(0, nl);
    lineAccum_.erase(0, nl + 1);
    if (!line.empty() && onLine_) onLine_(line);
  }
}

void DtraceCatch::parseFilter(const std::string& filter, std::string& provider, std::string& name) {
  // ""/"all" → syscall 全件 / "<名前>" → syscall::<名前>
  // "mach"/"mach:" → mach_trap 全件 / "mach:<名前>" → mach_trap::<名前>
  provider = "syscall";
  name = filter;
  if (filter == "mach" || filter.rfind("mach:", 0) == 0) {
    provider = "mach_trap";
    name = (filter.size() > 5) ? filter.substr(5) : "";
  }
  if (name == "all") name = "";
}

namespace {
std::string toLower(const std::string& s) {
  std::string r = s;
  for (char& c : r) c = (char)std::tolower((unsigned char)c);
  return r;
}
// dtrace_probe_iter のコールバック。arg は {小文字の部分一致キー, 出力先}。
struct ProbeCollect {
  std::string sub;                  // "" なら全件
  std::vector<std::string>* out;
};
int collectProbe(dtrace_hdl_t* h, const dtrace_probedesc_t* pd, void* arg) {
  (void)h;
  auto* c = static_cast<ProbeCollect*>(arg);
  // entry/return の両方が来るので entry だけ採る（func 名は同一）。
  if (std::strcmp(pd->dtpd_name, "entry") != 0) return 0;
  const char* f = pd->dtpd_func;
  if (!f || !*f || f[0] == '#') return 0;    // "#<番号>" は未実装スロット。除外。
  if (!c->sub.empty() && toLower(f).find(c->sub) == std::string::npos) return 0;
  c->out->push_back(f);
  return 0;
}
// 1 プロバイダ分を列挙して out に昇順・重複なしで詰める。
void iterProvider(dtrace_hdl_t* h, const char* spec, const std::string& sub,
                  std::vector<std::string>& out) {
  dtrace_probedesc_t pd;
  if (dtrace_str2desc(h, DTRACE_PROBESPEC_NAME, spec, &pd) != 0) return;
  ProbeCollect c{sub, &out};
  dtrace_probe_iter(h, &pd, collectProbe, &c);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}
} // namespace

bool DtraceCatch::listNames(const std::string& filter, std::vector<std::string>& bsd,
                            std::vector<std::string>& mach, std::string& err) {
  bsd.clear();
  mach.clear();
  // parseFilter と同じ入力規約で BSD / Mach のどちらを引くか、部分一致キーは何かを決める。
  bool wantBsd = true, wantMach = true;
  std::string sub = filter;
  if (filter == "mach" || filter.rfind("mach:", 0) == 0) {
    wantBsd = false;
    sub = (filter.size() > 5) ? filter.substr(5) : "";
  }
  if (sub == "all") sub = "";
  sub = toLower(sub);

  int derr = 0;
  dtrace_hdl_t* h = dtrace_open(DTRACE_VERSION, 0, &derr);
  if (!h) { err = std::string("dtrace_open 失敗: ") + dtrace_errmsg(nullptr, derr); return false; }
  if (wantBsd) iterProvider(h, "syscall:::entry", sub, bsd);
  if (wantMach) iterProvider(h, "mach_trap:::entry", sub, mach);
  dtrace_close(h);
  return true;
}

bool DtraceCatch::start(pid_t pid, const std::string& filter, Handler handler, std::string& err,
                        bool stopOnHit) {
  if (hdl_) { err = "既に監視中"; return false; }
  handler_ = std::move(handler);
  onLine_ = nullptr;
  stopReq_ = false;

  std::string provider, name;
  parseFilter(filter, provider, name);
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
  // 1 つも probe にマッチしないなら名前の誤りと見なして弾く（ZDEFS で許容すると黙って発火
  // しない監視が張られてしまうため）。syscall/mach_trap の probe は固定なので 0 件＝名前違い。
  if (info.dpi_matches == 0) {
    err = "該当する probe がありません（syscall/トラップ名を確認。例: write / mach:mach_msg_trap）";
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

bool DtraceCatch::startTrace(pid_t pid, const std::string& filter, LineHandler onLine,
                             std::string& err) {
  if (hdl_) { err = "既に監視中"; return false; }

  std::string provider, name;
  parseFilter(filter, provider, name);
  if (name.empty()) { err = "引数つき観測には syscall 名が必要（all は集計のみ）"; return false; }

  handler_ = nullptr;
  onLine_ = std::move(onLine);
  lineAccum_.clear();
  stopReq_ = false;

  // entry で引数を thread-local に退避 → return で引数・戻り値・errno を printf 整形。
  // quiet で既定ヘッダを消し、printf だけを dtrace_handle_buffered 経由で受け取る。
  std::string P = std::to_string((long long)pid);
  std::string pr = provider + "::" + name;
  std::string prog =
      "#pragma D option quiet\n" +
      pr + ":entry /pid == " + P + "/ { self->t = 1; self->a0 = arg0; self->a1 = arg1; "
           "self->a2 = arg2; self->a3 = arg3; }\n" +
      pr + ":return /pid == " + P + " && self->t/ { "
           "printf(\"%s(0x%x, 0x%x, 0x%x, 0x%x) = %d errno=%d\\n\", probefunc, "
           "self->a0, self->a1, self->a2, self->a3, (int)arg1, errno); self->t = 0; }";

  int derr = 0;
  dtrace_hdl_t* h = dtrace_open(DTRACE_VERSION, 0, &derr);
  if (!h) { err = std::string("dtrace_open 失敗: ") + dtrace_errmsg(nullptr, derr); return false; }
  dtrace_setopt(h, "bufsize", "4m");
  dtrace_setopt(h, "switchrate", "10hz");
  // printf の整形出力は dtrace_work に渡す FILE* に書かれる（buffered ハンドラは集計 printa 用で
  // plain printf では発火しない）。loop() が trace モードでは open_memstream を fp に使って拾う。
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
  // 1 つも probe にマッチしないなら名前の誤りと見なして弾く（ZDEFS で許容すると黙って発火
  // しない監視が張られてしまうため）。syscall/mach_trap の probe は固定なので 0 件＝名前違い。
  if (info.dpi_matches == 0) {
    err = "該当する probe がありません（syscall/トラップ名を確認。例: write / mach:mach_msg_trap）";
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
  if (g_dtDbg) fprintf(stderr, "[dt] trace go ok. prog=%s\n", prog.c_str());
  worker_ = std::thread([this] { loop(); });
  return true;
}

void DtraceCatch::loop() {
  auto* h = static_cast<dtrace_hdl_t*>(hdl_);
  const bool trace = (bool)onLine_; // startTrace で起動したか（引数つきトレース）
  // trace モードは printf 出力を拾うため open_memstream を fp に使う。それ以外は捨て先の devnull
  // （fp=NULL は内部でエラーになるため有効な FILE* が要る）。
  char* mbuf = nullptr; size_t msize = 0; size_t consumed = 0;
  FILE* fp = trace ? open_memstream(&mbuf, &msize) : fopen("/dev/null", "w");
  while (!stopReq_.load()) {
    dtrace_sleep(h); // switchrate 間隔で起きる（stop 要求に ~100ms で反応）
    dtrace_workstatus_t st = dtrace_work(h, fp, s_probe, s_rec, this);
    if (trace && fp) {
      fflush(fp); // dtrace_work が書いた printf 出力を確定
      if (msize > consumed) { cbBuffered(mbuf + consumed); consumed = msize; } // 末尾は null 終端
      // 全部消費し未完行も無ければ、肥大したストリームは作り直して捨てる（巻き戻しの size 挙動を避ける）
      if (consumed == msize && lineAccum_.empty() && msize > (1u << 20)) {
        fclose(fp); free(mbuf); mbuf = nullptr; msize = 0; consumed = 0;
        fp = open_memstream(&mbuf, &msize);
      }
    }
    if (g_dtDbg && st != DTRACE_WORKSTATUS_OKAY)
      fprintf(stderr, "[dt] work status=%d err=%s\n", (int)st,
              st == DTRACE_WORKSTATUS_ERROR ? dtrace_errmsg(h, dtrace_errno(h)) : "");
    if (st == DTRACE_WORKSTATUS_DONE || st == DTRACE_WORKSTATUS_ERROR) break;
  }
  if (fp) fclose(fp);
  free(mbuf);
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
