#pragma once

// L3 Presentation（ta2ui）: 端末制御（raw モード・代替画面・サイズ・位置指定描画）と表示幅ユーティリティ。
// v1 main.cpp 内の Terminal / layout を v2 用に最小構成で起こしたもの。ANSI を直接扱い、全角（CJK）は
// 2 桁として数える。フレームごとに putAt で 1 本のバッファへ積み、flush で一括出力する（ちらつき防止）。
#include <string>
#include <vector>

namespace ta2 {
namespace ui {

// 表示幅（半角=1 / 全角=2）。ANSI エスケープ（\x1b[...m / K / H）は 0 幅として数えない。
int displayWidth(const std::string& s);
// 表示幅ちょうど width 桁に切り詰め or 空白で右詰めする（ANSI はそのまま残す）。全角境界で割らない。
std::string fitWidth(const std::string& s, int width);

class Terminal {
public:
  // 代替画面に入り raw モードへ（カーソル非表示・画面消去）。元の termios を控える。
  void enter();
  // 元の画面・termios に戻す（カーソル表示）。
  void leave();
  bool entered() const { return entered_; }

  int rows() const { return rows_; }
  int cols() const { return cols_; }
  void refreshSize(); // TIOCGWINSZ でサイズ更新（SIGWINCH 後に呼ぶ）

  void clear();                                             // 画面全消去をバッファへ
  void clearLine(int row);                                 // 1 行消去をバッファへ
  void putAt(int col, int row, const std::string& s);      // 位置指定でバッファへ
  void flush();                                            // バッファを端末へ一括出力

private:
  bool entered_ = false;
  int rows_ = 24, cols_ = 80;
  std::string buf_;
  // 元 termios をバイト列で控える（.cpp で struct termios に復元）。macOS の termios は 72 バイト。
  unsigned char saved_[128] = {0};
  bool savedValid_ = false;
};

} // namespace ui
} // namespace ta2
