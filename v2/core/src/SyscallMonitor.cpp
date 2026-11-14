#include "core/SyscallMonitor.h"

namespace ta2 {

static const size_t kScTraceMax = 200; // トレース行のリングバッファ上限

bool SyscallMonitor::startWatch(const std::string& filter, Msg& err) {
  if (!target_.isAttached()) { err = Msg::of(MsgId::ErrNotAttached); return false; }
  stop(); // 二重起動を避ける（再開扱い）
  { std::lock_guard<std::mutex> lk(scMu_); scCounts_.clear(); scTrace_.clear(); scFilter_ = filter; }
  scTotal_.store(0);
  scStop_.store(false); // 観測モード

  std::string prov, name;
  DtraceCatch::parseFilter(filter, prov, name);
  if (!name.empty()) {
    // 特定の syscall/Mach トラップ: 引数・戻り値つきでトレースし、最近の呼び出しを保持する。
    // handler（consumer スレッド）は整形済み 1 行を受け取りリングバッファへ積むだけ。
    scTraceMode_.store(true);
    return dtrace_.startTrace(target_.pid(), filter, [this](const std::string& line) {
      scTotal_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lk(scMu_);
      scTrace_.push_back(line);
      if (scTrace_.size() > kScTraceMax) scTrace_.pop_front();
    }, err);
  }
  // 全件（all / mach 全件）: 名前別カウントのみ（全 syscall を引数つきで出すと洪水になるため）。
  scTraceMode_.store(false);
  return dtrace_.start(target_.pid(), filter, [this](const SyscallHit& h) {
    scTotal_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(scMu_);
    scCounts_[h.name]++;
  }, err);
}

std::vector<std::string> SyscallMonitor::trace() {
  std::lock_guard<std::mutex> lk(scMu_);
  return std::vector<std::string>(scTrace_.begin(), scTrace_.end());
}

bool SyscallMonitor::startStop(const std::string& filter, Msg& err) {
  if (!target_.isAttached()) { err = Msg::of(MsgId::ErrNotAttached); return false; }
  stop(); // 観測/別の停止キャッチポイントが動いていれば解除（dtrace ハンドルは 1 つ）
  { std::lock_guard<std::mutex> lk(scMu_); scCounts_.clear(); scFilter_ = filter; }
  scTotal_.store(0);
  scStop_.store(true); // 停止キャッチポイントモード
  // stopOnHit=true: D プログラムに stop() が入り、発火でカーネルが対象を凍結する。
  // handler（consumer スレッド）で通常停止へ合流させる。発火は 1 回/再開なので高頻度化しない。
  return dtrace_.start(target_.pid(), filter, [this](const SyscallHit& h) {
    scTotal_.fetch_add(1, std::memory_order_relaxed);
    stepper_.onExternalSuspend(h.name); // 既に kernel が対象を suspend 済み → 計上に取り込んで停止へ
  }, err, /*stopOnHit=*/true);
}

thread_act_t SyscallMonitor::firingThreadFor(const std::string& caught) {
  std::string want = caught;
  if (want.rfind("mach:", 0) == 0) want = want.substr(5); // "mach:" 前置を外す
  Arch& a = target_.arch();
  std::vector<uint8_t> ts(a.threadStateCount() * sizeof(natural_t), 0);
  for (auto th : target_.threads()) {
    if (!target_.getThreadState(th, ts.data())) continue;
    uint64_t pc = a.pc(ts.data());
    std::string fn = namer_ ? namer_(pc) : std::string(); // 例 "write+0xa" / "_kernelrpc_mach_port_allocate_trap+0xa"
    size_t plus = fn.find('+');
    std::string base = (plus == std::string::npos) ? fn : fn.substr(0, plus);
    if (base == want) return th;
  }
  return 0;
}

void SyscallMonitor::refineStopThread() {
  if (!stepper_.stopIsSyscallCatch() || !stepper_.stopped() || !target_.isAttached()) return;
  thread_act_t t = firingThreadFor(stepper_.syscallCaught());
  if (t == 0 || t == stepper_.stopThread()) return; // 見つからない／既に正しいなら暫定のまま
  stepper_.reselectStopThread(t);
}

void SyscallMonitor::stop() { dtrace_.stop(); scStop_.store(false); }

std::string SyscallMonitor::filter() {
  std::lock_guard<std::mutex> lk(scMu_);
  return scFilter_.empty() ? std::string("all") : scFilter_;
}

std::vector<std::pair<std::string, uint64_t>> SyscallMonitor::counts() {
  std::lock_guard<std::mutex> lk(scMu_);
  return std::vector<std::pair<std::string, uint64_t>>(scCounts_.begin(), scCounts_.end());
}

} // namespace ta2
