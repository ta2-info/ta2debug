#pragma once

// ユーザが手で付けるシンボル注釈（名前とメモ）。イメージ UUID ごとのテキストファイルに保存する。
//  - 条件1: 素のテキスト（TAB 区切り）なので、普通のエディタで手編集できる。
//  - 条件2: デバッガ起動中にファイルが更新されていたら mtime を見て自動で読み直す。
//  - 条件3: デバッガのコマンド（ann/memo/unann）でも追記・編集できる。
// アドレスは unslid（slide を抜いた静的アドレス＝Mach-O 上の vmaddr）で保存するので、
// 再実行や ASLR を跨いでも有効。Mach-O はアーキ非依存なので Arch 依存なし。
// 用途は「自作バイナリに自分で名前／メモを足す」こと。シンボル表（Symbols）には手を入れず、
// こちらを重ね合わせる（byName/byAddress のオーバーレイ）。
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ImageList.h"
#include "core/Symbols.h" // Symbol

namespace ta2 {

struct Annot {
  uint64_t unslid = 0;  // 静的アドレス（実行時アドレス = unslid + img.slide）
  std::string name;     // 付けた名前（空可: メモだけの注釈）
  std::string memo;     // 自由記述メモ（空可）
};

class Annotations {
public:
  // 保存ディレクトリ ~/Library/Application Support/ta2debug/annotations（無ければ作成）。HOME 不明なら空。
  // キャッシュ（Caches）ではなくユーザデータ扱い＝消去対象にしない場所に置く。
  static std::string dir();
  // そのイメージの注釈ファイルのパス（<dir>/<UUID>.t2a）。UUID が無ければ空。
  static std::string pathFor(const Image& img);

  // img の注釈ファイルを、未読なら読み、mtime が変わっていれば読み直す（条件2）。
  // 合成シンボルを作り直したら true。※メインスレッドの境界（コマンド／停止時）からのみ呼ぶこと。
  bool refresh(const Image& img);

  // ---- 重ね合わせ用（Symbols から呼ぶ。読むだけ） ----
  // 名前の完全一致で合成シンボルを返す（byName 相当）。ポインタは本クラスが保持。
  std::vector<const Symbol*> byName(const Image& img, const std::string& name);
  // addr（実行時）以下で最も近い注釈シンボル（byAddress のオーバーレイ用）。無ければ nullptr。
  const Symbol* nearestSym(const Image& img, uint64_t addr);
  // addr（実行時）に完全一致する注釈（メモ表示用）。無ければ nullptr。
  const Annot* atExact(const Image& img, uint64_t addr);

  // ---- 編集（コマンドから。実行時 addr を受け取り unslid に直して保存。条件3） ----
  bool setName(const Image& img, uint64_t addr, const std::string& name, std::string& err);
  bool setMemo(const Image& img, uint64_t addr, const std::string& memo, std::string& err);
  bool remove(const Image& img, uint64_t addr, std::string& err);

  // 表示用の一覧（現在読み込んでいる注釈。unslid 昇順）。無ければ空。
  std::vector<Annot> list(const Image& img);

private:
  struct Entry {
    std::string path;
    time_t mtime = 0;
    long size = -1;      // ファイルサイズ（同一秒内の変更も拾う補助）
    bool loaded = false;
    uint64_t slide = 0;              // syms を作ったときの slide
    std::vector<Annot> annots;       // unslid 昇順
    std::vector<Symbol> syms;        // 合成（addr=unslid+slide, mangled=name）。名前付きのみ。addr 昇順
  };
  static std::string uuidHex(const Image& img); // 大文字 32 桁（ファイル名用）
  Entry* find(const Image& img);                // 読み込み済み Entry（無ければ nullptr）
  Entry& getOrLoad(const Image& img);           // 無ければ読み込む
  void rebuildSyms(Entry& e);
  static bool parseFile(const std::string& path, std::vector<Annot>& out);
  bool writeFile(Entry& e, const Image& img, std::string& err); // 保存して mtime 更新

  std::mutex mu_;
  std::unordered_map<std::string, Entry> byUuid_; // キー=UUID 32桁大文字
};

} // namespace ta2
