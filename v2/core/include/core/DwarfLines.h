#pragma once

// DWARF 行番号情報（.debug_line）を解釈し、実行時アドレス → ソース「ファイル:行」を返す。
// 取得元は 2 通り: (1) 隣接する dSYM（UUID 一致）、(2) dSYM が無ければ -g ビルド本体の
// debug map（LC_SYMTAB の STABS: N_OSO/N_FUN）を辿り、各 .o に残る DWARF を読んで
// 関数ごとのアドレス差分で unslid 実アドレスへ変換する（dsymutil と同じ原理の簡易版）。
// comp_dir は .debug_info（最小限）から取り、可能なら絶対パスを解決してソーステキストも読む。
// 外部ライブラリは使わず自前でパース（eh_frame 等と同じ方針）。Mach-O はアーキ非依存。
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ta2 {

struct DwarfSections; // __DWARF セクション群（実体は .cpp）。dSYM / .o 双方で使う。

struct SourceLoc {
  std::string file;   // 表示用の名前（多くは basename、例 "dsp.c"）
  int line = 0;
  std::string path;   // 解決できた絶対パス（読めない場合は空）
  std::string text;   // そのファイル行のソーステキスト（読めた場合）
};

class DwarfLines {
public:
  // imagePath の隣の dSYM を探し、UUID 一致ならロードする。成功で true。
  bool loadForImage(const std::string& imagePath, const uint8_t uuid[16], uint64_t slide);
  bool loaded() const { return loaded_; }

  // 実行時アドレス → ソース位置。見つかれば true。
  bool lookup(uint64_t runtimeAddr, SourceLoc& out) const;

  // path のソース line 行目（1 始まり）を返す。読めなければ空。結果はキャッシュ。
  const std::string& sourceText(const std::string& path, int line);

private:
  struct Row { uint64_t addr; int fileIdx; int line; bool endSeq; };
  bool loaded_ = false;
  uint64_t slide_ = 0;
  std::vector<Row> rows_;                 // addr 昇順
  std::vector<std::string> fileLabels_;   // fileIdx → 表示名
  std::vector<std::string> filePaths_;    // fileIdx → 絶対パス（無ければ空）
  std::unordered_map<std::string, int> intern_; // "label\x01path" → fileIdx（複数 .o で共有）

  // s の .debug_line を解釈し rows_ へ追加する。xlate(rawAddr, isEndSeq, &out) は
  // 行プログラム中のアドレスを最終的な unslid アドレスへ変換し、採用するなら true を返す
  // （dSYM は恒等変換、debug map は関数ごとの差分を加える／マップ外の行は false で捨てる）。
  void parseLineSections(const DwarfSections& s,
                         const std::function<bool(uint64_t, bool, uint64_t&)>& xlate);
  // dSYM が無いとき、imagePath 本体の debug map を辿って各 .o の DWARF を取り込む。
  bool loadViaDebugMap(const std::string& imagePath, const uint8_t uuid[16]);

  // ソースファイルの行キャッシュ: path → 行配列（1 始まりを 0 始まりで保持）
  std::unordered_map<std::string, std::vector<std::string>> srcCache_;
  std::string empty_;
};

} // namespace ta2
