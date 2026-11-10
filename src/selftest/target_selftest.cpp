// Target のセルフテスト。引数で渡された pid にアタッチし、スレッド列挙・レジスタ取得・
// メモリ R/W（データ領域とコード領域の両方）を検証する。task_for_pid のため root 実行が要る。
//
// 使い方（テストハーネスが行う）:
//   target_child が "DATA=<addr> CODE=<addr> VAL=<hex>" を出力 → その pid と各値を渡す
//     target_selftest <pid> <dataAddr> <codeAddr> <expectVal>
#include "core/Target.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ta2;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? "OK" : "NG", what);
  if (!ok) failures++;
}

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s <pid> <dataAddr> <codeAddr> <expectVal>\n", argv[0]); return 2; }
  pid_t pid = (pid_t)strtol(argv[1], nullptr, 10);
  uint64_t dataAddr = strtoull(argv[2], nullptr, 0);
  uint64_t codeAddr = strtoull(argv[3], nullptr, 0);
  uint64_t expect   = strtoull(argv[4], nullptr, 0);

  Target target;
  std::string err;
  if (!target.attach(pid, err)) { printf("attach 失敗: %s\n", err.c_str()); return 1; }
  printf("attached pid=%d arch=%s\n", target.pid(), target.arch().name());

  // スレッド列挙
  auto th = target.threads();
  printf("threads: %zu\n", th.size());
  check(!th.empty(), "スレッドが 1 本以上");

  // レジスタ取得（PC が 0 でない）
  if (!th.empty()) {
    std::vector<uint8_t> ts(target.arch().threadStateCount() * sizeof(natural_t), 0);
    bool got = target.getThreadState(th[0], ts.data());
    uint64_t pc = got ? target.arch().pc(ts.data()) : 0;
    printf("thread0 pc=0x%llx\n", (unsigned long long)pc);
    check(got && pc != 0, "レジスタ取得・PC!=0");
  }

  // データ領域を読む（子が印字した値と一致するか）
  uint64_t v = 0;
  bool rd = target.readMemory(dataAddr, &v, sizeof(v));
  printf("read data @0x%llx = 0x%llx (expect 0x%llx)\n",
         (unsigned long long)dataAddr, (unsigned long long)v, (unsigned long long)expect);
  check(rd && v == expect, "データ領域を読めて値が一致");

  // データ領域へ書いて読み戻す
  uint64_t w = 0xA5A5A5A5A5A5A5A5ULL;
  bool wr = target.writeMemory(dataAddr, &w, sizeof(w));
  uint64_t back = 0;
  target.readMemory(dataAddr, &back, sizeof(back));
  check(wr && back == w, "データ領域へ書いて読み戻せる");
  target.writeMemory(dataAddr, &expect, sizeof(expect)); // 元へ戻す

  // コード領域へ書く（CoW + 保護復元の経路。1 byte 書いて読み戻し、元へ戻す）
  uint8_t orig = 0;
  target.readMemory(codeAddr, &orig, 1);
  uint8_t bp = target.arch().softBreakBytes()[0];
  bool cwr = target.writeMemory(codeAddr, &bp, 1);
  uint8_t cback = 0;
  target.readMemory(codeAddr, &cback, 1);
  check(cwr && cback == bp, "コード領域へ書ける（CoW 経路）");
  target.writeMemory(codeAddr, &orig, 1); // 元の命令へ戻す
  uint8_t restored = 0;
  target.readMemory(codeAddr, &restored, 1);
  check(restored == orig, "コード領域を元へ戻せる");

  target.detach();
  check(!target.isAttached(), "detach 後は未アタッチ");

  printf(failures == 0 ? "\nALL OK\n" : "\n%d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
