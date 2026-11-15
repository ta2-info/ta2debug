#include "ui/Terminal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#if defined(__APPLE__)
#include <Carbon/Carbon.h> // TIS 入力ソース（システムフレームワーク。依存は増やさない）
#endif

namespace ta2 {
namespace ui {

namespace {
#if defined(__APPLE__)
// TUI 起動時に一度だけ IME を半角（ASCII 可能なキーボードレイアウト）へ切り替える。日本語入力の
// まま起動してもコマンドが全角で弾かれないように。以後は固定しない（メモ入力等で全角に戻せる）。
// best-effort で、失敗・GUI セッション無しでも無視する。
void switchToAsciiInputSourceOnce() {
  TISInputSourceRef ascii = TISCopyCurrentASCIICapableKeyboardInputSource();
  if (!ascii) return;
  TISSelectInputSource(ascii);
  CFRelease(ascii);
}
#else
void switchToAsciiInputSourceOnce() {}
#endif
} // namespace
namespace {
// UTF-8 の 1 文字を取り出し、コードポイントとバイト数を返す。壊れていれば 1 バイトとして進める。
uint32_t decodeUtf8(const std::string& s, size_t i, int& len) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) { len = 1; return c; }
  if ((c >> 5) == 0x6 && i + 1 < s.size()) { len = 2; return ((c & 0x1f) << 6) | (s[i+1] & 0x3f); }
  if ((c >> 4) == 0xe && i + 2 < s.size()) { len = 3; return ((c & 0x0f) << 12) | ((s[i+1] & 0x3f) << 6) | (s[i+2] & 0x3f); }
  if ((c >> 3) == 0x1e && i + 3 < s.size()) { len = 4; return ((c & 0x07) << 18) | ((s[i+1] & 0x3f) << 12) | ((s[i+2] & 0x3f) << 6) | (s[i+3] & 0x3f); }
  len = 1; return c;
}
// おおまかな East-Asian Wide 判定（全角＝2 桁）。CJK・かな・全角記号・絵文字域を広めに拾う。
int cpWidth(uint32_t cp) {
  if (cp == 0) return 0;
  if ((cp >= 0x1100 && cp <= 0x115F) ||  // Hangul Jamo
      (cp >= 0x2E80 && cp <= 0x303E) ||  // CJK Radicals … 記号
      (cp >= 0x3041 && cp <= 0x33FF) ||  // かな・CJK 記号
      (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK Ext A
      (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK 統合漢字
      (cp >= 0xA000 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) ||  // Hangul
      (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK 互換
      (cp >= 0xFE30 && cp <= 0xFE4F) ||
      (cp >= 0xFF00 && cp <= 0xFF60) ||  // 全角英数記号
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F300 && cp <= 0x1FAFF) || // 絵文字
      (cp >= 0x20000 && cp <= 0x3FFFD))   // CJK Ext B+
    return 2;
  return 1;
}
// 位置 i が ANSI エスケープ（\x1b[ … 最終バイト）なら、その全長を返す。違えば 0。
size_t ansiLen(const std::string& s, size_t i) {
  if (i + 1 >= s.size() || s[i] != '\x1b' || s[i+1] != '[') return 0;
  size_t j = i + 2;
  while (j < s.size() && !((s[j] >= '@' && s[j] <= '~'))) j++;
  return (j < s.size()) ? (j - i + 1) : (s.size() - i);
}
} // namespace

int displayWidth(const std::string& s) {
  int w = 0;
  for (size_t i = 0; i < s.size();) {
    if (size_t a = ansiLen(s, i)) { i += a; continue; }
    int len = 1; uint32_t cp = decodeUtf8(s, i, len);
    w += cpWidth(cp); i += len;
  }
  return w;
}

std::string fitWidth(const std::string& s, int width) {
  if (width < 0) width = 0;
  std::string out; int w = 0;
  for (size_t i = 0; i < s.size();) {
    if (size_t a = ansiLen(s, i)) { out.append(s, i, a); i += a; continue; } // ANSI は幅 0 で素通し
    int len = 1; uint32_t cp = decodeUtf8(s, i, len);
    int cw = cpWidth(cp);
    if (w + cw > width) break;                 // これ以上は入らない（全角は割らない）
    out.append(s, i, len); w += cw; i += len;
  }
  for (; w < width; w++) out += ' ';            // 右を空白で埋める
  return out;
}

void Terminal::enter() {
  struct termios t;
  if (tcgetattr(STDIN_FILENO, &t) == 0) {
    static_assert(sizeof(struct termios) <= sizeof(saved_), "termios が saved_ に収まらない");
    memcpy(saved_, &t, sizeof(t));
    savedValid_ = true;
    struct termios raw = t;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }
  refreshSize();
  ensureCells();
  switchToAsciiInputSourceOnce(); // 起動時だけ IME を半角へ（固定はしない）
  buf_ = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H"; // 代替画面・カーソル非表示・全消去・原点
  flush();
  entered_ = true;
}

void Terminal::setMouse(bool on) {
  mouseOn_ = on;
  buf_ += on ? "\x1b[?1000;1002;1006h" : "\x1b[?1000;1002;1006l";
  flush();
}

void Terminal::leave() {
  if (!entered_) return;
  if (mouseOn_) { buf_ += "\x1b[?1000;1002;1006l"; mouseOn_ = false; } // マウス報告を戻す
  buf_ += "\x1b[?25h\x1b[?1049l"; // カーソル表示・通常画面へ
  flush();
  if (savedValid_) { struct termios t; memcpy(&t, saved_, sizeof(t)); tcsetattr(STDIN_FILENO, TCSANOW, &t); }
  entered_ = false;
}

void Terminal::refreshSize() {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
    rows_ = ws.ws_row; cols_ = ws.ws_col;
  }
}

void Terminal::ensureCells() {
  if ((int)cells_.size() != rows_) cells_.assign(rows_, std::vector<std::string>(cols_, " "));
  for (auto& r : cells_) if ((int)r.size() != cols_) r.assign(cols_, " ");
}

void Terminal::clear() { buf_ += "\x1b[2J\x1b[H"; ensureCells(); for (auto& r : cells_) std::fill(r.begin(), r.end(), std::string(" ")); }
void Terminal::clearLine(int row) {
  char b[32]; snprintf(b, sizeof(b), "\x1b[%d;1H\x1b[2K", row + 1); buf_ += b;
  ensureCells(); if (row >= 0 && row < rows_) std::fill(cells_[row].begin(), cells_[row].end(), std::string(" "));
}

void Terminal::putAt(int col, int row, const std::string& s) {
  char b[32]; snprintf(b, sizeof(b), "\x1b[%d;%dH", row + 1, col + 1);
  buf_ += b; buf_ += s; buf_ += "\x1b[K"; // 行末までクリア（残骸消し）
  // コピー用セルモデルも更新（ANSI 除去・全角 2 セル）。行末までクリア相当に残りも空白化。
  ensureCells();
  if (row < 0 || row >= rows_) return;
  int c = col;
  for (size_t i = 0; i < s.size();) {
    if (size_t a = ansiLen(s, i)) { i += a; continue; }
    int len = 1; uint32_t cp = decodeUtf8(s, i, len); int w = cpWidth(cp);
    if (c >= 0 && c < cols_) cells_[row][c] = s.substr(i, len);
    if (w == 2 && c + 1 >= 0 && c + 1 < cols_) cells_[row][c + 1] = "";
    c += (w > 0 ? w : 1); i += len;
  }
  for (; c < cols_; c++) if (c >= 0) cells_[row][c] = " "; // 行末までクリア
}

std::string Terminal::copyRegion(int r0, int c0, int r1, int c1) const {
  if (r0 > r1 || (r0 == r1 && c0 > c1)) { std::swap(r0, r1); std::swap(c0, c1); }
  std::string out;
  for (int r = r0; r <= r1 && r < (int)cells_.size(); r++) {
    if (r < 0) continue;
    int a = (r == r0) ? c0 : 0;
    int b = (r == r1) ? c1 : cols_ - 1;
    std::string line;
    for (int c = std::max(0, a); c <= b && c < cols_; c++) line += cells_[r][c];
    while (!line.empty() && line.back() == ' ') line.pop_back(); // 末尾空白を除く
    out += line; if (r < r1) out += "\n";
  }
  return out;
}

void Terminal::flush() {
  if (!buf_.empty()) { ssize_t n = write(STDOUT_FILENO, buf_.data(), buf_.size()); (void)n; buf_.clear(); }
}

} // namespace ui
} // namespace ta2
