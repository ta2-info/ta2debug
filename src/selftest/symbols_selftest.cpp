// ImageList / Symbols のセルフテスト。対象にアタッチし、イメージ一覧を取り、
// メイン実行ファイルの関数シンボルと、指定アドレス（worker）→名前解決を確認する。root 実行が要る。
//   symbols_selftest <pid> <workerAddr> <workerName>
#include "core/Target.h"
#include "core/ImageList.h"
#include "core/Symbols.h"

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
  if (argc < 4) { fprintf(stderr, "usage: %s <pid> <workerAddr> <workerName>\n", argv[0]); return 2; }
  pid_t pid = (pid_t)strtol(argv[1], nullptr, 10);
  uint64_t workerAddr = strtoull(argv[2], nullptr, 0);
  std::string workerName = argv[3];

  Target target;
  std::string err;
  if (!target.attach(pid, err)) { printf("attach 失敗: %s\n", err.c_str()); return 1; }

  ImageList images(target);
  Symbols symbols(target);
  if (!images.reload()) { printf("image 列挙失敗\n"); return 1; }

  printf("images: %zu\n", images.images().size());
  check(images.images().size() >= 2, "イメージが複数（本体 + dyld/libsystem 等）");

  const Image* main = images.mainExecutable();
  check(main != nullptr, "メイン実行ファイルを特定");
  if (main) printf("main = %s loadAddr=0x%llx slide=0x%llx\n",
                   main->name.c_str(), (unsigned long long)main->loadAddr, (unsigned long long)main->slide);

  // worker アドレスを含むイメージ
  const Image* owner = images.byAddress(workerAddr);
  check(owner != nullptr && owner == main, "worker アドレスのイメージがメイン実行ファイル");

  // worker アドレス → 名前
  if (owner) {
    const Symbol* s = symbols.byAddress(*owner, workerAddr);
    printf("byAddress(0x%llx) = %s%s\n", (unsigned long long)workerAddr,
           s ? symbols.displayName(*s).c_str() : "(なし)",
           (s && s->addr == workerAddr) ? " (+0)" : "");
    check(s && symbols.displayName(*s) == workerName && s->addr == workerAddr, "worker の名前・アドレス一致");

    // 名前 → アドレス
    auto hits = symbols.byName(*owner, workerName);
    check(!hits.empty() && hits[0]->addr == workerAddr, "名前からアドレスを引ける");
  }

  // dlopen 検知（stamp 変化が無ければ false が返る）
  bool changed = images.refreshIfChanged();
  printf("refreshIfChanged (変化なし想定) = %s\n", changed ? "true" : "false");

  target.detach();
  printf(failures == 0 ? "\nALL OK\n" : "\n%d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
