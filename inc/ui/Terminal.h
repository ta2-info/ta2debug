#pragma once

// 端末制御。raw モード、代替画面、カーソル表示/非表示、(col,row) への出力、行/全消去。
// 旧版 Term 相当の最小機能。アーキには依存しない。
#include <cstdint>
#include <string>
#include <termios.h>

namespace ta2 {

class Terminal {
public:
  Terminal() = default;
  ~Terminal();

  void enter();   // raw + 代替画面 + カーソル非表示
  void leave();   // 元に戻す（冪等）

  int rows() const { return rows_; }
  int cols() const { return cols_; }
  void querySize();

  void clear();
  void clearLine(int row);
  void moveTo(int col, int row);        // 0 始まり
  void put(const std::string& s);
  void putAt(int col, int row, const std::string& s);
  void flush();

private:
  bool active_ = false;
  struct termios saved_{};
  int rows_ = 24;
  int cols_ = 80;
};

} // namespace ta2
