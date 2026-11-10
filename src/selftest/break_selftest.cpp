// エンドツーエンドのセルフテスト:
//   対象にアタッチ → 関数先頭にソフトブレイク → 例外を受信 → レジスタ取得 → 逆アセンブル表示
//   → ブレイクを外して対象を続行 → デタッチ。
// task_for_pid と例外ポート差し替えのため root 実行が要る。
//   break_selftest <pid> <breakAddr>
#include "core/Target.h"
#include "core/Breakpoints.h"
#include "core/ExceptionServer.h"
#include "core/Disassembler.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

using namespace ta2;

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <pid> <breakAddr>\n", argv[0]); return 2; }
  pid_t pid = (pid_t)strtol(argv[1], nullptr, 10);
  uint64_t bpAddr = strtoull(argv[2], nullptr, 0);

  Target target;
  std::string err;
  if (!target.attach(pid, err)) { printf("attach 失敗: %s\n", err.c_str()); return 1; }
  printf("attached pid=%d arch=%s  break@0x%llx\n", target.pid(), target.arch().name(),
         (unsigned long long)bpAddr);

  Breakpoints bps(target);

  std::atomic<bool> hit{false};
  uint64_t hitPc = 0;
  std::vector<uint8_t> hitState(target.arch().threadStateCount() * sizeof(natural_t), 0);

  ExceptionServer es;
  auto handler = [&](thread_act_t thread, exception_type_t exc,
                     const int64_t* /*codes*/, int /*n*/) -> kern_return_t {
    if (exc == EXC_BREAKPOINT && !hit) {
      std::vector<uint8_t> ts(target.arch().threadStateCount() * sizeof(natural_t), 0);
      if (target.getThreadState(thread, ts.data())) {
        uint64_t pc = target.arch().pc(ts.data());
        uint64_t at = pc - target.arch().softBreakPcBackup();
        if (bps.has(at)) {
          bps.removeSoft(at);                 // 元の命令へ戻す
          target.arch().setPc(ts.data(), at); // その命令から再実行させる
          target.setThreadState(thread, ts.data());
          hitPc = at;
          hitState = ts;
          hit = true;
        }
      }
    }
    return KERN_SUCCESS; // 対象を再開
  };

  if (!es.install(target.task(), handler, err)) { printf("例外ポート差し替え失敗: %s\n", err.c_str()); return 1; }
  if (!bps.addSoft(bpAddr)) { printf("ブレイク設定失敗\n"); es.uninstall(); return 1; }
  printf("ブレイク設定完了。ヒット待ち...\n");

  // 対象がブレイク箇所を通るまで待つ（最大 5 秒）
  for (int i = 0; i < 500 && !hit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int rc = 0;
  if (!hit) {
    printf("NG: 5 秒以内にヒットしなかった\n");
    rc = 1;
  } else {
    printf("\n=== ブレイク！ rip/pc = 0x%llx %s ===\n", (unsigned long long)hitPc,
           hitPc == bpAddr ? "(= break addr, OK)" : "(≠ break addr, NG)");
    if (hitPc != bpAddr) rc = 1;

    // レジスタ表示（Arch が決めた一覧から数本）
    auto regs = target.arch().registers(hitState.data());
    printf("--- registers (%zu 本) ---\n", regs.size());
    for (size_t i = 0; i < regs.size() && i < 8; i++)
      printf("  %-6s = 0x%016llx\n", regs[i].name.c_str(), (unsigned long long)regs[i].value);

    // 逆アセンブル表示（ブレイク地点から 6 命令）
    printf("--- disassemble @0x%llx ---\n", (unsigned long long)hitPc);
    uint8_t code[64] = {};
    size_t got = 0;
    target.readMemory(hitPc, code, sizeof(code), &got);
    Disassembler dis(target.arch());
    auto insns = dis.disasm(code, got, hitPc, 6);
    if (insns.empty()) { printf("  (デコードできなかった)\n"); rc = 1; }
    for (auto& in : insns) {
      const char* k = in.kind == InsnKind::Call ? " ; call"
                    : in.kind == InsnKind::Return ? " ; ret"
                    : in.kind == InsnKind::Syscall ? " ; syscall" : "";
      printf("  0x%llx  %-8s %s%s\n", (unsigned long long)in.address,
             in.mnemonic.c_str(), in.opStr.c_str(), k);
    }
  }

  es.uninstall();
  bps.removeAll();
  target.detach();
  printf("\n%s\n", rc == 0 ? "ALL OK" : "FAILED");
  return rc;
}
