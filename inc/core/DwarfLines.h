#pragma once

// dSYM の DWARF 行番号情報（.debug_line）を解釈し、実行時アドレス → ソース「ファイル:行」を返す。
// comp_dir は .debug_info（最小限）から取り、可能なら絶対パスを解決してソーステキストも読む。
// 外部ライブラリは使わず自前でパース（eh_frame 等と同じ方針）。Mach-O はアーキ非依存。
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ta2 {

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

  // ソースファイルの行キャッシュ: path → 行配列（1 始まりを 0 始まりで保持）
  std::unordered_map<std::string, std::vector<std::string>> srcCache_;
  std::string empty_;
};

} // namespace ta2
