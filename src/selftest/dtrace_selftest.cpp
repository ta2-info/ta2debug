// DtraceCatch（libdtrace 相乗りの syscall 検知）の単体テスト。root が要る。
// 子プロセスを fork→自分で SIGSTOP→親が監視開始→SIGCONT で走らせ、子の write を数える。
#include "core/DtraceCatch.h"

#include <atomic>
#include <cstdio>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

int main() {
  if (geteuid() != 0) { fprintf(stderr, "root 権限で実行してください\n"); return 2; }

  const int kWrites = 20;
  pid_t pid = fork();
  if (pid == 0) {
    raise(SIGSTOP); // 親が監視を仕掛けるまで待つ（取りこぼし防止）
    int fd = open("/dev/null", O_WRONLY);
    for (int i = 0; i < kWrites; i++) { write(fd, "x", 1); usleep(20 * 1000); }
    _exit(0);
  }
  if (pid < 0) { perror("fork"); return 1; }

  int st = 0;
  waitpid(pid, &st, WUNTRACED); // 子が SIGSTOP で止まるのを待つ

  ta2::DtraceCatch dt;
  std::atomic<int> writes{0}, total{0};
  std::string err;
  if (!dt.start(pid, "write", [&](const ta2::SyscallHit& h) {
        total++;
        if (h.name == "write") writes++;
      }, err)) {
    fprintf(stderr, "DtraceCatch::start 失敗: %s\n", err.c_str());
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return 1;
  }

  kill(pid, SIGCONT);        // 子を走らせる
  waitpid(pid, &st, 0);      // 子の終了を待つ
  usleep(300 * 1000);        // consumer がバッファを吸い出す猶予
  dt.stop();

  printf("captured write=%d total=%d (expected ~%d)\n", writes.load(), total.load(), kWrites);
  bool ok = writes.load() >= kWrites / 2;
  printf(ok ? "DTRACE SELFTEST OK\n" : "DTRACE SELFTEST FAIL\n");
  return ok ? 0 : 1;
}
