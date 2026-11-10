#include "core/Target.h"

#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <crt_externs.h> // _NSGetEnviron

#include <mach/mach_vm.h>

#ifndef POSIX_SPAWN_START_SUSPENDED
#define POSIX_SPAWN_START_SUSPENDED 0x0080 // Apple 拡張（走り出す前に task_suspend 済みで生成）
#endif
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100 // Apple 拡張: ASLR 無効化（dyld スライド 0 でロード）
#endif

namespace ta2 {

Target::~Target() {
  detach();
}

bool Target::attach(pid_t pid, std::string& err) {
  if (task_ != 0) { err = "すでにアタッチしている"; return false; }

  task_t task = 0;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
  if (kr != KERN_SUCCESS) {
    err = std::string("task_for_pid 失敗: ") + mach_error_string(kr) +
          "（root 実行か、SIP 無効か、対象が自作バイナリかを確認）";
    return false;
  }

  pid_ = pid;
  task_ = task;
  suspendCount_ = 0;
  // 対象アーキの決定。いまはホストと同一（x86_64 機では x86_64、arm64 機では arm64）。
  // TODO: arm64 機での Rosetta(x86_64) 対象など、混在時はメイン Mach-O の cputype で判定する。
  arch_ = Arch::createForHost();
  return true;
}

bool Target::launch(const std::string& path, char* const argv[], std::string& err) {
  if (task_ != 0) { err = "すでにアタッチしている"; return false; }

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
  if (rc != 0) { err = std::string("posix_spawn 失敗: ") + strerror(rc); return false; }

  task_t task = 0;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task); // START_SUSPENDED なので走る前に取れる
  if (kr != KERN_SUCCESS) {
    err = std::string("task_for_pid 失敗: ") + mach_error_string(kr);
    kill(pid, SIGKILL);
    return false;
  }
  pid_ = pid;
  task_ = task;
  suspendCount_ = 1; // START_SUSPENDED のぶんを自分の停止として数える（resume で解除される）
  arch_ = Arch::createForHost();
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
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    natural_t depth = 0;
    for (;;) {
      vm_region_submap_info_data_64_t info = {};
      mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
      kern_return_t kr = mach_vm_region_recurse(task_, &a, &sz, &depth,
                                                (vm_region_recurse_info_t)&info, &cnt);
      if (kr != KERN_SUCCESS || a > addr) return false;
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
