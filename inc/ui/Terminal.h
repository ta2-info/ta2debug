#pragma once

// 端末制御。raw モード、代替画面、カーソル表示/非表示、(col,row) への出力、行/全消去。
// 旧版 Term 相当の最小機能。アーキには依存しない。
// さらに、描いた可視文字を「セル格子（シャドウバッファ）」に控えて、マウスドラッグによる
// 自前のテキスト選択（反転ハイライト描画＋クリップボードへコピー）を実現する。
#include <cstdint>
#include <string>
#include <termios.h>
#include <vector>

namespace ta2 {

class Terminal {
public:
  Terminal() = default;
  ~Terminal();

  void enter();   // raw + 代替画面 + カーソル非表示 + マウス通知
  void leave();   // 元に戻す（冪等）

  void setMouse(bool on); // マウス通知（SGR 1000/1002/1006）を実行中に ON/OFF。OFF 中は端末標準のドラッグ選択が戻る
  bool mouseOn() const { return mouse_; }

  // ---- マウスによる自前テキスト選択 ----
  void selStart(int col, int row);       // ドラッグ開始（アンカー固定）。0 始まり
  void selUpdate(int col, int row);      // ドラッグ中/終了（現在位置を更新）
  void clearSelection();                 // 選択解除
  bool hasSelection() const { return selActive_; }
  void drawSelectionOverlay();           // 直近の描画の上に反転ハイライトを重ねる（render 末尾で呼ぶ）
  std::string selectedText() const;      // 選択範囲の可視文字（行末空白は落とし、改行で連結）
  bool copySelection();                  // selectedText() を pbcopy と OSC52 でクリップボードへ

  int rows() const { return rows_; }
  int cols() const { return cols_; }
  void querySize();

  void clear();         // 画面全消去＋原点（\x1b[2J\x1b[H）。残渣は消えるが一瞬ブランクになる
  void home();          // 原点復帰のみ（\x1b[H）。消さずに上書き描画してチラつきを避ける
  void clearLine(int row);
  void moveTo(int col, int row);        // 0 始まり
  void put(const std::string& s);
  void putAt(int col, int row, const std::string& s); // 可視文字はセル格子にも控える
  void flush();

private:
  void ensureGrid();                     // rows_×cols_ に合わせて格子を確保/クリア
  void gridPut(int col, int row, const std::string& s); // putAt の控え（可視文字をセルへ）

  bool active_ = false;
  bool mouse_ = false;
  struct termios saved_{};
  int rows_ = 24;
  int cols_ = 80;

  // セル格子: 各セルに UTF-8 1 文字。全角の右側セルは "" (継続)、未書き込み/空白は " "。
  std::vector<std::string> cells_;
  int gridRows_ = 0, gridCols_ = 0;

  // 選択範囲（画面セル, 0 始まり, 読み順で [anchor..cursor]）。
  bool selActive_ = false;
  int selAr_ = 0, selAc_ = 0;   // アンカー
  int selCr_ = 0, selCc_ = 0;   // 現在位置
};

} // namespace ta2
