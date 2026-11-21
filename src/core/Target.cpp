#include "core/Target.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>         // getgrouplist / setgroups（子の降格用）
#include <pwd.h>         // getpwuid（呼び出しユーザの HOME/補助グループ解決）
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>    // waitpid（降格失敗した子の後始末）
#include <termios.h>
#include <unistd.h>
#include <util.h>        // openpty
#include <vector>
#include <crt_externs.h> // _NSGetEnviron

#include <mach/mach_vm.h>

#ifndef POSIX_SPAWN_START_SUSPENDED
#define POSIX_SPAWN_START_SUSPENDED 0x0080 // Apple 拡張（走り出す前に task_suspend 済みで生成）
#endif
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100 // Apple 拡張: ASLR 無効化（dyld スライド 0 でロード）
#endif
#ifndef POSIX_SPAWN_SETEXEC
#define POSIX_SPAWN_SETEXEC 0x0040 // Apple 拡張: fork せず呼び出しプロセスをその場で exec（降格後に使う）
#endif
#ifndef NGROUPS_MAX
#define NGROUPS_MAX 16 // setgroups が受け付ける補助グループ数の上限（macOS）
#endif

namespace ta2 {

// sudo 実行時、デバッガが作るログファイル <path>.ta2debug.log は root 所有になってしまう。
// 呼び出しユーザ（SUDO_UID/GID）所有へ戻して、後からユーザが読める／消せるようにする（best-effort。
// Annotations / BreakpointStore と同方針）。
static void chownLogToInvoker(const std::string& path) {
  if (geteuid() != 0) return;                 // root でなければ何もしない
  const char* su = getenv("SUDO_UID");
  const char* sg = getenv("SUDO_GID");
  if (!su || !*su) return;                    // sudo 経由でなければ対象外
  uid_t uid = (uid_t)strtoul(su, nullptr, 10);
  gid_t gid = (sg && *sg) ? (gid_t)strtoul(sg, nullptr, 10) : (gid_t)-1;
  if (uid == 0) return;
  (void)chown(path.c_str(), uid, gid);
}

namespace {

// launch で起動する子を「呼び出しユーザ」権限へ降格するための情報。
// デバッガ自身（親）は task_for_pid のため root を保つが、デバッグ対象の子まで root で
// 走らせるのは過剰で、ファイル所有や権限チェックの挙動が普段の実行と変わってしまう。
// そこで sudo 実行時だけ、SUDO_UID/GID の一般ユーザへ子を落とす。
struct DropInfo {
  bool want = false;
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<gid_t> groups; // 補助グループ（root のそれを引きずらないよう付け替える）
  std::string home, name;    // 子の HOME/USER/LOGNAME を普段どおりにするため
};

// 降格情報を「親・fork 前」で全部そろえる。fork 後の子では getpwuid/getgrouplist/setenv の
// ような async-signal-safe でない API を使えない（マルチスレッド親からの fork でデッドロックし得る）
// ため、DB 参照やメモリ確保を伴うものはここで済ませ、子では syscall だけを呼ぶ。
DropInfo computeInvokerDrop() {
  DropInfo d;
  if (geteuid() != 0) return d;                 // root でなければ降格不要（entitlement 運用など）
  const char* su = getenv("SUDO_UID");
  const char* sg = getenv("SUDO_GID");
  if (!su || !*su) return d;                    // sudo 経由でなければ対象外
  uid_t uid = (uid_t)strtoul(su, nullptr, 10);
  if (uid == 0) return d;                        // 降格先が root では意味がない
  gid_t gid = (sg && *sg) ? (gid_t)strtoul(sg, nullptr, 10) : (gid_t)uid;
  d.want = true; d.uid = uid; d.gid = gid;
  if (struct passwd* pw = getpwuid(uid)) {
    if (pw->pw_dir)  d.home = pw->pw_dir;
    if (pw->pw_name) d.name = pw->pw_name;
    if (!d.name.empty()) {                        // 補助グループ一覧を getgrouplist で取得
      int ng = 32; std::vector<int> g(ng);
      if (getgrouplist(d.name.c_str(), (int)gid, g.data(), &ng) < 0 && ng > 0) {
        g.assign(ng, 0);
        getgrouplist(d.name.c_str(), (int)gid, g.data(), &ng);
      }
      // macOS の setgroups は NGROUPS_MAX(=16) までしか受け付けない（getgrouplist はそれ以上
      // 返し得る）。超過分で setgroups が EINVAL になるので先頭 NGROUPS_MAX 件に丸める。
      if (ng > NGROUPS_MAX) ng = NGROUPS_MAX;
      for (int i = 0; i < ng; i++) d.groups.push_back((gid_t)g[i]);
    }
  }
  return d;
}

// 子へ渡す環境を用意する。降格時は HOME/USER/LOGNAME を呼び出しユーザのものへ差し替え、
// 普段ユーザが起動したときと同じ素性にする（デバッグ対象が getenv("HOME") 等を見る場合の対策）。
// 返り値の char* は envStore 内の文字列実体を指すので、envStore を spawn 完了まで生かすこと。
std::vector<char*> buildChildEnv(const DropInfo& d, std::vector<std::string>& envStore) {
  auto isvar = [](const char* e, const char* k) {
    size_t n = strlen(k); return strncmp(e, k, n) == 0 && e[n] == '=';
  };
  for (char** e = *_NSGetEnviron(); e && *e; ++e) {
    if (d.want && (isvar(*e, "HOME") || isvar(*e, "USER") || isvar(*e, "LOGNAME"))) continue;
    envStore.push_back(*e);
  }
  if (d.want) {
    if (!d.home.empty()) envStore.push_back("HOME=" + d.home);
    if (!d.name.empty()) { envStore.push_back("USER=" + d.name); envStore.push_back("LOGNAME=" + d.name); }
  }
  std::vector<char*> envp;
  envp.reserve(envStore.size() + 1);
  for (auto& s : envStore) envp.push_back(s.data()); // 全 push_back 後に取得（再確保後なので安全）
  envp.push_back(nullptr);
  return envp;
}

} // namespace

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

  // sudo 実行時は子を呼び出しユーザへ降格する（情報は fork 前にそろえる）。
  DropInfo drop = computeInvokerDrop();

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  // START_SUSPENDED: 走り出す前に止めて生成。DISABLE_ASLR: スライドを 0 に固定し、
  // 再実行のたびに同じアドレスにする（lldb/debugserver の既定と同じ。ブレイクやログ突合が再現可能になる）。
  // 降格時は fork 済みの子でその場 exec するため SETEXEC も立てる。
  short spawnFlags = POSIX_SPAWN_START_SUSPENDED | _POSIX_SPAWN_DISABLE_ASLR;
  if (drop.want) spawnFlags |= POSIX_SPAWN_SETEXEC;
  posix_spawnattr_setflags(&attr, spawnFlags);

  // 子の標準出力/エラーは TUI を汚さないようログファイルへ。ただしレギュラーファイルへ直結すると
  // 子 libc が「端末ではない」と判断して全バッファ（4〜8KB 単位）になり、ブレイクで止めている間は
  // printf/dump の出力がまだ子のユーザ空間バッファに溜まったままでログに現れない。そこで子へは
  // PTY スレーブを渡す：子からは stdout が端末に見えて行バッファになり、改行ごとに即出る。親は
  // PTY マスタを読んで <path>.ta2debug.log に書き写す（これなら tail -f で逐次見える）。
  std::string logpath = path + ".ta2debug.log";
  // ガード: 生成するログ名のファイル名部分が '-' 始まり（または空）だと、後でシェルから
  // オプションと誤認されて rm で消しにくくなる（rm -- / ./ 前置が要る）。安全な接頭辞 '_' を
  // 挟んでハイフン始まりを避ける（例: 対象が "-" → "_-.ta2debug.log"）。ディレクトリ部は保つ。
  {
    size_t slash = logpath.find_last_of('/');
    size_t base = (slash == std::string::npos) ? 0 : slash + 1;
    if (base >= logpath.size() || logpath[base] == '-') logpath.insert(base, "_");
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);

  int master = -1, slave = -1;
  bool usePty = (openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
  if (usePty) {
    // 端末の出力加工（OPOST/ONLCR の \n→\r\n 変換、エコー等）を切り、子の出力バイトを無改変で記録。
    struct termios t;
    if (tcgetattr(slave, &t) == 0) { cfmakeraw(&t); tcsetattr(slave, TCSANOW, &t); }
    posix_spawn_file_actions_adddup2(&fa, slave, 1);
    posix_spawn_file_actions_adddup2(&fa, slave, 2);
    posix_spawn_file_actions_addclose(&fa, slave);  // dup 後に子側のスレーブ原本を閉じる
    posix_spawn_file_actions_addclose(&fa, master); // 子はマスタを持たない
  } else {
    // PTY を用意できないときは従来どおりログファイルへ直接（全バッファのまま）。
    posix_spawn_file_actions_addopen(&fa, 1, logpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
  }

  // 子へ渡す環境（降格時は HOME/USER/LOGNAME を呼び出しユーザへ）。envStore を spawn まで生かす。
  std::vector<std::string> envStore;
  std::vector<char*> envp = buildChildEnv(drop, envStore);

  pid_t pid = 0;
  int rc = 0, childStage = 0; // childStage: 1=setgroups 2=setgid 3=setuid 4=exec（降格経路の失敗箇所）
  if (drop.want) {
    // fork → 子で一般ユーザへ降格 → SETEXEC でその場に対象を exec。親は root を保ち task_for_pid できる。
    // 子の降格/exec 失敗は self-pipe（CLOEXEC）で親へ通知: 成功 exec なら write 端が閉じて親は EOF を受ける。
    int efd[2] = {-1, -1};
    if (pipe(efd) == 0) { fcntl(efd[1], F_SETFD, FD_CLOEXEC); fcntl(efd[0], F_SETFD, FD_CLOEXEC); }
    pid = fork();
    if (pid == 0) {                              // ---- 子（fork 後は syscall のみ）----
      if (efd[0] >= 0) close(efd[0]);
      auto fail = [&](int stage) {
        int e = errno;
        if (efd[1] >= 0) { (void)!write(efd[1], &stage, sizeof(stage)); (void)!write(efd[1], &e, sizeof(e)); }
        _exit(127);
      };
      if (!drop.groups.empty() && setgroups((int)drop.groups.size(), drop.groups.data()) != 0) fail(1);
      if (setgid(drop.gid) != 0) fail(2);
      if (setuid(drop.uid) != 0) fail(3);
      posix_spawn(nullptr, path.c_str(), &fa, &attr, argv, envp.data()); // SETEXEC: 失敗時のみ戻る
      fail(4);
    }
    // ---- 親 ----
    if (efd[1] >= 0) close(efd[1]);
    if (pid < 0) {
      rc = errno;                                // fork 失敗
      if (efd[0] >= 0) close(efd[0]);
    } else if (efd[0] >= 0) {
      int stage = 0, cerr = 0;
      ssize_t r1 = read(efd[0], &stage, sizeof(stage)); // 成功 exec なら CLOEXEC で write 端が閉じ EOF(=0)
      if (r1 == (ssize_t)sizeof(stage)) { (void)!read(efd[0], &cerr, sizeof(cerr)); rc = cerr ? cerr : EPERM; childStage = stage; }
      close(efd[0]);
    }
  } else {
    rc = posix_spawn(&pid, path.c_str(), &fa, &attr, argv, envp.data());
  }
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (usePty) close(slave); // 親はスレーブ不要。閉じないと子終了時もマスタが EOF にならない
  if (rc != 0 || pid <= 0) {
    if (usePty) close(master);
    if (pid > 0) { kill(pid, SIGKILL); int st; waitpid(pid, &st, 0); } // 降格失敗の子を始末
    const char* st = childStage == 1 ? "setgroups" : childStage == 2 ? "setgid"
                   : childStage == 3 ? "setuid"    : childStage == 4 ? "exec" : "spawn";
    err = std::string("子プロセス起動に失敗 (") + st + "): " + strerror(rc ? rc : ECHILD);
    return false;
  }

  task_t task = 0;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task); // START_SUSPENDED なので走る前に取れる
  if (kr != KERN_SUCCESS) {
    err = std::string("task_for_pid 失敗: ") + mach_error_string(kr);
    if (usePty) close(master);
    kill(pid, SIGKILL);
    return false;
  }
  pid_ = pid;
  task_ = task;
  suspendCount_ = 1; // START_SUSPENDED のぶんを自分の停止として数える（resume で解除される）
  arch_ = Arch::createForHost();
  launched_ = true;

  // PTY マスタ → ログファイルのポンプ。read で受けたぶんをそのまま write するので stdio バッファを
  // 挟まず、子が改行で flush した行が即ファイルへ届く（＝ tail -f で逐次見える）。子が終了して
  // スレーブが全て閉じるとマスタ read が EOF/EIO になり、スレッドは自然に終わる（detach で join）。
  if (usePty) {
    ptyMaster_ = master;
    logFd_ = open(logpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rfd = master, wfd = logFd_;
    ptyPump_ = std::thread([rfd, wfd]() {
      char buf[4096];
      for (;;) {
        ssize_t n = read(rfd, buf, sizeof(buf));
        if (n > 0) {
          for (ssize_t off = 0; wfd >= 0 && off < n; ) {
            ssize_t w = write(wfd, buf + off, (size_t)(n - off));
            if (w <= 0) { if (errno == EINTR) continue; break; }
            off += w;
          }
        } else if (n == 0) {
          break;                              // EOF
        } else {
          if (errno == EINTR) continue;
          break;                              // EIO 等（子が終了してスレーブが閉じた）
        }
      }
    });
  }
  // PTY 版のログは親（root）が open するので root 所有のまま。呼び出しユーザ所有へ戻す。
  // （非 PTY＋降格経路では子＝ユーザが addopen するので元からユーザ所有。chown は冪等で無害。）
  chownLogToInvoker(logpath);
  return true;
}

void Target::detach() {
  if (task_ == 0) { teardownPty(); return; }
  while (suspendCount_ > 0) { task_resume(task_); suspendCount_--; }
  pid_t p = pid_; bool launched = launched_;
  mach_port_deallocate(mach_task_self(), task_);
  task_ = 0;
  pid_ = 0;
  arch_.reset();
  launched_ = false;
  if (launched) kill(p, SIGKILL); // 自分で起動した子はデバッガ終了時に始末する
  teardownPty();                  // 子を終わらせた後に PTY ポンプを止めて片付ける
}

// 起動した子の PTY ポンプを止めて fd を閉じる。子は既に終了/kill 済みの前提：スレーブが閉じて
// マスタ read が EOF/EIO になりポンプスレッドが抜けるので join できる。attach のみ（pty 無し）や
// 二重呼び出しでも安全（何も無ければ即 return）。
void Target::teardownPty() {
  if (ptyPump_.joinable()) ptyPump_.join();
  if (ptyMaster_ >= 0) { close(ptyMaster_); ptyMaster_ = -1; }
  if (logFd_ >= 0) { close(logFd_); logFd_ = -1; }
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
