// v1 の src/core/Target.cpp の移植。差分は 2 点:
//   ・エラーを std::string から Msg へ（設計書 §8.1。strerror / mach_error_string は {detail}）。
//   ・attach / launch で対象の cpu type を確認し、このビルドの Arch で扱えなければ
//     err.target.arch_mismatch で拒否する（設計書 §9.1。v1 は無検査で createForHost()）。
#include "platform/Target.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <crt_externs.h> // _NSGetEnviron
#include <sys/sysctl.h>

#include <mach/mach_vm.h>

#ifndef POSIX_SPAWN_START_SUSPENDED
#define POSIX_SPAWN_START_SUSPENDED 0x0080 // Apple 拡張（走り出す前に task_suspend 済みで生成）
#endif
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100 // Apple 拡張: ASLR 無効化（dyld スライド 0 でロード）
#endif

namespace ta2 {

namespace {
// 対象プロセスの cpu type を返す（sysctl "sysctl.proc_cputype"。Rosetta 下の x86_64 プロセスは
// arm64 機でも CPU_TYPE_X86_64 が返る）。取得できなければ false（その場合は検査を省く）。
bool processCpuType(pid_t pid, cpu_type_t& out) {
  int mib[CTL_MAXNAME] = {};
  size_t len = CTL_MAXNAME;
  if (sysctlnametomib("sysctl.proc_cputype", mib, &len) != 0 || len >= CTL_MAXNAME) return false;
  mib[len] = pid;
  cpu_type_t cpu = 0;
  size_t sz = sizeof(cpu);
  if (sysctl(mib, (u_int)(len + 1), &cpu, &sz, nullptr, 0) != 0 || sz != sizeof(cpu)) return false;
  out = cpu;
  return true;
}

// エラー表示用の cpu type 名（識別子なので翻訳対象外）。
std::string cpuTypeName(cpu_type_t cpu) {
  switch (cpu) {
    case CPU_TYPE_X86_64: return "x86_64";
    case CPU_TYPE_ARM64:  return "arm64";
    case CPU_TYPE_X86:    return "i386";
    case CPU_TYPE_ARM:    return "arm";
    default: { char b[32]; snprintf(b, sizeof(b), "cpu_type 0x%x", (unsigned)cpu); return b; }
  }
}

// 対象 pid に合う Arch を決める。cpu type が取れなければ v1 と同じくホスト実装を使う。
// 取れたのにこのビルドに実装が無い（= ホストと違うアーキ）なら nullptr を返し err を埋める。
std::unique_ptr<Arch> archForProcess(pid_t pid, Msg& err) {
  cpu_type_t cpu = 0;
  if (!processCpuType(pid, cpu)) return Arch::createForHost();
  std::unique_ptr<Arch> a = Arch::create(cpu);
  if (!a) {
    err = Msg::of(MsgId::ErrTargetArchMismatch)
              .str("target", cpuTypeName(cpu))
              .str("host", Arch::createForHost()->name());
  }
  return a;
}
} // namespace

Target::~Target() {
  detach();
}

bool Target::attach(pid_t pid, Msg& err) {
  if (task_ != 0) { err = Msg::of(MsgId::ErrTargetAlreadyAttached); return false; }

  task_t task = 0;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
  if (kr != KERN_SUCCESS) {
    err = Msg::of(MsgId::ErrTaskForPid).str("detail", mach_error_string(kr));
    return false;
  }

  // 対象アーキの決定。ホスト = ターゲット前提（設計書 §9.1）。arm64 機での Rosetta(x86_64)
  // 対象など、このビルドの Arch で扱えないものは掴まずに拒否する（誤ったレジスタ配置で触らない）。
  std::unique_ptr<Arch> arch = archForProcess(pid, err);
  if (!arch) {
    mach_port_deallocate(mach_task_self(), task);
    return false;
  }

  pid_ = pid;
  task_ = task;
  suspendCount_ = 0;
  arch_ = std::move(arch);
  return true;
}

bool Target::launch(const std::string& path, char* const argv[], Msg& err) {
  if (task_ != 0) { err = Msg::of(MsgId::ErrTargetAlreadyAttached); return false; }

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  // START_SUSPENDED: 走り出す前に止めて生成。DISABLE_ASLR: スライドを 0 に固定し、
  // 再実行のたびに同じアドレスにする（lldb/debugserver の既定と同じ。ブレイクやログ突合が再現可能になる）。
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_START_SUSPENDED | _POSIX_SPAWN_DISABLE_ASLR);

  // 子の標準出力/エラーは TUI を汚さないようログファイルへ
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  std::string logpath = path + ".ta2debug.log";
  posix_spawn_file_actions_addopen(&fa, 1, logpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);

  pid_t pid = 0;
  char** envp = *_NSGetEnviron();
  int rc = posix_spawn(&pid, path.c_str(), &fa, &attr, argv, envp);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) { err = Msg::of(MsgId::ErrPosixSpawn).str("detail", strerror(rc)); return false; }

  task_t task = 0;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task); // START_SUSPENDED なので走る前に取れる
  if (kr != KERN_SUCCESS) {
    err = Msg::of(MsgId::ErrTaskForPidLaunch).str("detail", mach_error_string(kr));
    kill(pid, SIGKILL);
    return false;
  }
  // 起動した子のアーキを確認（attach と同じ。設計書 §9.1）。扱えなければ子を始末して拒否。
  std::unique_ptr<Arch> arch = archForProcess(pid, err);
  if (!arch) {
    mach_port_deallocate(mach_task_self(), task);
    kill(pid, SIGKILL);
    return false;
  }
  pid_ = pid;
  task_ = task;
  suspendCount_ = 1; // START_SUSPENDED のぶんを自分の停止として数える（resume で解除される）
  arch_ = std::move(arch);
  launched_ = true;
  return true;
}

void Target::detach() {
  if (task_ == 0) return;
  while (suspendCount_ > 0) { task_resume(task_); suspendCount_--; }
  pid_t p = pid_; bool launched = launched_;
  mach_port_deallocate(mach_task_self(), task_);
  task_ = 0;
  pid_ = 0;
  arch_.reset();
  launched_ = false;
  if (launched) kill(p, SIGKILL); // 自分で起動した子はデバッガ終了時に始末する
}

bool Target::suspend() {
  if (task_ == 0) return false;
  if (task_suspend(task_) != KERN_SUCCESS) return false;
  suspendCount_++;
  return true;
}

bool Target::resume() {
  if (task_ == 0 || suspendCount_ == 0) return false;
  if (task_resume(task_) != KERN_SUCCESS) return false;
  suspendCount_--;
  return true;
}

void Target::adoptExternalSuspend() {
  if (task_ == 0) return;
  // task は既に外部（dtrace の stop() アクション）で suspend 済み。task_suspend は呼ばず、
  // 計上だけ合わせる。以後の resume()（continue）1 回で kernel の suspend_count が 0 に戻る。
  suspendCount_++;
}

void Target::cancelExternalSuspend() {
  if (task_ == 0) return;
  // 既に自分が停止中に外部サスペンドが 1 段乗った場合に、その 1 段だけ解除する。
  // 取り込んでいない（suspendCount_ に数えていない）ので計上はいじらない。
  task_resume(task_);
}

bool Target::readMemory(uint64_t addr, void* buf, size_t size, size_t* outRead) {
  if (task_ == 0) return false;
  mach_vm_size_t got = 0;
  kern_return_t kr = mach_vm_read_overwrite(task_, addr, size, (mach_vm_address_t)buf, &got);
  if (outRead) *outRead = (size_t)got;
  return kr == KERN_SUCCESS && got == size;
}

bool Target::writeMemory(uint64_t addr, const void* buf, size_t size) {
  if (task_ == 0) return false;

  // 実ページの保護属性を取る。共有キャッシュはサブマップなので region_recurse で中まで降りる
  //（mach_vm_region だとサブマップ全体の値が返り、戻すと実行権限が消えて対象が落ちる）。
  vm_prot_t prot = VM_PROT_NONE;
  {
    mach_vm_size_t sz = 0;
    natural_t depth = 0;
    for (;;) {
      // 必ず「対象アドレス」で問い合わせる。recurse は見つけた領域の先頭に a を書き換えるので、
      // submap を 1 段降りるたびに a を addr へ戻さないと、次の問い合わせが submap の先頭から
      // 始まり「addr を含まない別の leaf」の保護値を拾ってしまう。すると復元時にその誤った保護を
      // 適用して対象ページの実行権限を剥奪し、関数実行で EXC_BAD_ACCESS になる（printf 等で発生）。
      mach_vm_address_t a = addr;
      vm_region_submap_info_data_64_t info = {};
      mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
      kern_return_t kr = mach_vm_region_recurse(task_, &a, &sz, &depth,
                                                (vm_region_recurse_info_t)&info, &cnt);
      // addr を含む領域（base <= addr < base+size）であることを上限まで厳密に確認する。
      // 上限チェックが無いと、base <= addr だが addr より手前で終わる領域を誤採用してしまう。
      if (kr != KERN_SUCCESS || a > addr || addr >= a + sz) return false;
      if (info.is_submap) { depth++; continue; }
      prot = info.protection;
      break;
    }
  }

  bool changed = false;
  if (!(prot & VM_PROT_WRITE)) {
    // コードページ(r-x)は max_protection に WRITE が無い。VM_PROT_COPY で CoW のプライベートコピーにする。
    if (mach_vm_protect(task_, addr, size, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
      return false;
    }
    changed = true;
  }

  kern_return_t kr = mach_vm_write(task_, addr, (vm_offset_t)buf, (mach_msg_type_number_t)size);
  bool ok = (kr == KERN_SUCCESS);

  if (changed) {
    mach_vm_protect(task_, addr, size, FALSE, prot); // 元の保護へ戻す（r-x に戻し実行可能に）
  }
  return ok;
}

std::vector<thread_act_t> Target::threads() {
  std::vector<thread_act_t> out;
  if (task_ == 0) return out;
  thread_act_array_t arr = nullptr;
  mach_msg_type_number_t n = 0;
  if (task_threads(task_, &arr, &n) != KERN_SUCCESS) return out;
  for (mach_msg_type_number_t i = 0; i < n; i++) out.push_back(arr[i]);
  vm_deallocate(mach_task_self(), (vm_address_t)arr, n * sizeof(thread_act_t));
  return out;
}

bool Target::getThreadState(thread_act_t t, void* buf) {
  mach_msg_type_number_t cnt = arch_->threadStateCount();
  return thread_get_state(t, arch_->threadFlavor(), (thread_state_t)buf, &cnt) == KERN_SUCCESS;
}

bool Target::getFloatState(thread_act_t t, std::vector<uint8_t>& out) {
  thread_state_flavor_t flavor = arch_->floatFlavor();
  if (!flavor) return false; // このアーキは未対応
  mach_msg_type_number_t cnt = arch_->floatStateCount();
  out.assign((size_t)cnt * sizeof(natural_t), 0);
  return thread_get_state(t, flavor, (thread_state_t)out.data(), &cnt) == KERN_SUCCESS;
}

bool Target::setThreadState(thread_act_t t, const void* buf) {
  return thread_set_state(t, arch_->threadFlavor(), (thread_state_t)buf,
                          arch_->threadStateCount()) == KERN_SUCCESS;
}

bool Target::getDebugState(thread_act_t t, void* buf) {
  mach_msg_type_number_t cnt = arch_->debugStateCount();
  return thread_get_state(t, arch_->debugFlavor(), (thread_state_t)buf, &cnt) == KERN_SUCCESS;
}

bool Target::setDebugState(thread_act_t t, const void* buf) {
  return thread_set_state(t, arch_->debugFlavor(), (thread_state_t)buf,
                          arch_->debugStateCount()) == KERN_SUCCESS;
}

} // namespace ta2
