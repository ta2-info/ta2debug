#pragma once

// イメージごとのシンボル解決。LC_SYMTAB(nlist_64) を対象メモリの __LINKEDIT から読み、
// C++ 名はデマングルする。アドレス→名前（二分探索）と名前→アドレスを提供し、UUID でキャッシュ。
// Mach-O はアーキ非依存なので Arch 依存なし。
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ImageList.h"
#include "core/Target.h"

namespace ta2 {

struct Symbol {
  uint64_t addr = 0;              // 実行時アドレス（slide 適用済み）
  std::string mangled;           // 先頭 '_' を除いたもの
  mutable std::string demangled; // 遅延デマングル結果のキャッシュ（空=未計算）。displayName() 経由で埋める。
};

class Symbols {
public:
  explicit Symbols(Target& t) : target_(t) {}

  const std::vector<Symbol>& forImage(const Image& img);
  // addr を含む（addr 以下で最も近い）シンボル。無ければ nullptr。
  const Symbol* byAddress(const Image& img, uint64_t addr);
  // 名前一致（mangled 完全一致 / デマングル名 / 引数を除いた名前）。候補は mangled 部分一致で絞ってから
  // デマングルするので、巨大なシンボルテーブルでも全件デマングルしない（重い処理を避ける）。
  std::vector<const Symbol*> byName(const Image& img, const std::string& name);
  // 表示名（必要になった時だけデマングルしてキャッシュ）。メインスレッドからのみ呼ぶ。
  const std::string& displayName(const Symbol& s);

  // addr が __stubs 内なら、そのスタブが指す本来のシンボル名（先頭 '_' を除く。例 "getpid"）を返す。
  // 間接シンボルテーブル経由で解決。スタブ外・未解決なら空文字。
  std::string stubTarget(const Image& img, uint64_t addr);

  // ディスクキャッシュの保存先（~/Library/Caches/ta2debug/syms）。無ければ作成。
  static std::string cacheDir();

private:
  std::string keyFor(const Image& img);
  void load(const Image& img, std::vector<Symbol>& out);
  // 隣接する dSYM（<path>.dSYM/…）があり UUID が一致すれば、その symtab を out へ追加する。
  // stripped バイナリでも関数名が得られる。見つからなければ何もしない。
  void loadDsymSymbols(const Image& img, std::vector<Symbol>& out);
  // UUID をキーにしたディスクキャッシュ（更新で UUID が変われば別ファイル＝自動で作り直し）。
  std::string cachePath(const Image& img);
  bool loadFromDisk(const Image& img, std::vector<Symbol>& out);
  void saveToDisk(const Image& img, const std::vector<Symbol>& syms);

  Target& target_;
  std::mutex mu_; // cache_ とディスク I/O を保護（バックグラウンド warming と共用するため）
  std::unordered_map<std::string, std::vector<Symbol>> cache_;
};

} // namespace ta2
