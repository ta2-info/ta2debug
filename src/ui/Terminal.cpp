#include "ui/Terminal.h"

#include <cstdio>
#include <unistd.h>
#include <sys/ioctl.h>

#include <Carbon/Carbon.h> // TIS 入力ソース（システムフレームワーク。依存は増やさない）

namespace ta2 {

namespace {
// TUI 起動時に一度だけ IME を半角（ASCII 可能なキーボードレイアウト）へ切り替える。
// 日本語入力モードのまま起動してもコマンドが全角になって弾かれないようにするため。
// あくまで「起動時に一度」だけで、以後は固定しない（ユーザが全角へ自由に戻せる＝メモ入力等で
// 全角を使える）。best-effort で、失敗しても無視する（端末のみ・GUI セッションが無ければ何も起きない）。
// TISCopyCurrentASCIICapableKeyboardInputSource は日本語 IME ではなく ASCII 可能な
// 「キーボードレイアウト」を返すので、選択すると確実に半角英数になる。
void switchToAsciiInputSourceOnce() {
  TISInputSourceRef ascii = TISCopyCurrentASCIICapableKeyboardInputSource();
  if (!ascii) return;
  TISSelectInputSource(ascii);
  CFRelease(ascii);
}
} // namespace

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
  switchToAsciiInputSourceOnce(); // 起動時だけ IME を半角へ（固定はしない）
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
void Terminal::home() { put("\x1b[H"); }

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
