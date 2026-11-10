#include "ui/Terminal.h"

#include <cstdio>
#include <unistd.h>
#include <sys/ioctl.h>

namespace ta2 {

Terminal::~Terminal() { leave(); }

void Terminal::querySize() {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
    rows_ = ws.ws_row;
    cols_ = ws.ws_col;
  }
}

void Terminal::enter() {
  if (active_) return;
  tcgetattr(STDIN_FILENO, &saved_);
  struct termios raw = saved_;
  raw.c_lflag &= ~(ICANON | ECHO | ISIG);
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  querySize();
  put("\x1b[?1049h"); // 代替画面
  put("\x1b[?25l");   // カーソル非表示
  clear();
  flush();
  active_ = true;
}

void Terminal::leave() {
  if (!active_) return;
  put("\x1b[?25h");   // カーソル表示
  put("\x1b[?1049l"); // 代替画面を抜ける
  flush();
  tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  active_ = false;
}

void Terminal::clear() { put("\x1b[2J\x1b[H"); }

void Terminal::clearLine(int row) {
  moveTo(0, row);
  put("\x1b[2K");
}

void Terminal::moveTo(int col, int row) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row + 1, col + 1);
  put(buf);
}

void Terminal::put(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }
void Terminal::putAt(int col, int row, const std::string& s) { moveTo(col, row); put(s); }
void Terminal::flush() { fflush(stdout); }

} // namespace ta2
