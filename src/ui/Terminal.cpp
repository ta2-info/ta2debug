#include "ui/Terminal.h"
#include "ui/Layout.h"

#include <cstdio>
#include <unistd.h>
#include <sys/ioctl.h>
#include <algorithm>

#include <Carbon/Carbon.h> // TIS 入力ソース（システムフレームワーク。依存は増やさない）

namespace ta2 {

namespace {
// 選択テキストを OSC 52 で送るための base64。小さいので自前（依存を増やさない）。
std::string base64(const std::string& in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out; out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2];
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
  }
  if (i + 1 == in.size()) {
    unsigned v = (unsigned char)in[i] << 16;
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == in.size()) {
    unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8);
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
  }
  return out;
}
} // namespace

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
  setMouse(true);     // マウス通知を ON（:mouse でいつでも OFF にできる）
}

// マウス通知の ON/OFF（実行中に切り替え可能）。
//  ON : 1000h=押下/解放 ＋ 1002h=ボタン押下中の移動（ドラッグ選択に使う） ＋ 1006h=SGR 拡張座標。
//  OFF: すべて解除 → 端末標準のドラッグ選択・コピーが戻る。
void Terminal::setMouse(bool on) {
  if (on) { put("\x1b[?1000h"); put("\x1b[?1002h"); put("\x1b[?1006h"); }
  else    { put("\x1b[?1006l"); put("\x1b[?1002l"); put("\x1b[?1000l"); }
  flush();
  mouse_ = on;
}

void Terminal::leave() {
  if (!active_) return;
  if (mouse_) setMouse(false); // マウス通知を解除（通常の選択/スクロールへ戻す）
  put("\x1b[?25h");   // カーソル表示
  put("\x1b[?1049l"); // 代替画面を抜ける
  flush();
  tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  active_ = false;
}

void Terminal::clear() {
  put("\x1b[2J\x1b[H");
  ensureGrid();
  std::fill(cells_.begin(), cells_.end(), std::string(" ")); // 格子も空白に
}
void Terminal::home() { put("\x1b[H"); }

void Terminal::clearLine(int row) {
  moveTo(0, row);
  put("\x1b[2K");
  ensureGrid();
  if (row >= 0 && row < gridRows_)
    for (int c = 0; c < gridCols_; c++) cells_[row * gridCols_ + c] = " ";
}

void Terminal::moveTo(int col, int row) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row + 1, col + 1);
  put(buf);
}

void Terminal::put(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }
void Terminal::putAt(int col, int row, const std::string& s) {
  moveTo(col, row);
  put(s);
  gridPut(col, row, s); // 可視文字をセル格子へ控える（選択・コピー用）
}
void Terminal::flush() { fflush(stdout); }

// ---- セル格子（シャドウバッファ） ----
void Terminal::ensureGrid() {
  if (gridRows_ == rows_ && gridCols_ == cols_ && (int)cells_.size() == rows_ * cols_) return;
  gridRows_ = rows_; gridCols_ = cols_;
  cells_.assign((size_t)rows_ * cols_, std::string(" "));
}

void Terminal::gridPut(int col, int row, const std::string& s) {
  ensureGrid();
  if (row < 0 || row >= gridRows_) return;
  int c = col;
  for (auto& gw : layout::glyphs(s)) {
    const std::string& g = gw.first; int w = gw.second;
    if (g == "\r" || g == "\n") break;
    if (c >= 0 && c < gridCols_) cells_[row * gridCols_ + c] = g;
    if (w == 2 && c + 1 >= 0 && c + 1 < gridCols_) cells_[row * gridCols_ + c + 1] = ""; // 右側=継続
    c += (w > 0 ? w : 1);
  }
  // 行末まで消去（\x1b[K / \x1b[0K）が含まれていたら、残りセルを空白に
  if (s.find("\x1b[K") != std::string::npos || s.find("\x1b[0K") != std::string::npos)
    for (int cc = c; cc < gridCols_; cc++) cells_[row * gridCols_ + cc] = " ";
}

// ---- マウス選択 ----
void Terminal::selStart(int col, int row) {
  selActive_ = true; selAr_ = row; selAc_ = col; selCr_ = row; selCc_ = col;
}
void Terminal::selUpdate(int col, int row) {
  if (!selActive_) { selStart(col, row); return; }
  selCr_ = row; selCc_ = col;
}
void Terminal::clearSelection() { selActive_ = false; }

// 読み順で (begin<=end) を返す
void selOrdered(int ar, int ac, int cr, int cc, int& br, int& bc, int& er, int& ec) {
  if (ar < cr || (ar == cr && ac <= cc)) { br = ar; bc = ac; er = cr; ec = cc; }
  else { br = cr; bc = cc; er = ar; ec = ac; }
}

void Terminal::drawSelectionOverlay() {
  if (!selActive_ || cells_.empty()) return;
  int br, bc, er, ec; selOrdered(selAr_, selAc_, selCr_, selCc_, br, bc, er, ec);
  br = std::max(0, std::min(br, gridRows_ - 1));
  er = std::max(0, std::min(er, gridRows_ - 1));
  for (int r = br; r <= er; r++) {
    int cs = (r == br) ? bc : 0;
    int ce = (r == er) ? ec : gridCols_ - 1;
    cs = std::max(0, std::min(cs, gridCols_ - 1));
    ce = std::max(0, std::min(ce, gridCols_ - 1));
    if (cs > ce) continue;
    moveTo(cs, r);
    put("\x1b[7m"); // 反転
    for (int c = cs; c <= ce; c++) {
      const std::string& g = cells_[r * gridCols_ + c];
      if (g.empty()) continue;           // 全角の継続セルは左側で描画済み
      put(g);
    }
    put("\x1b[0m");
  }
}

std::string Terminal::selectedText() const {
  if (!selActive_ || cells_.empty()) return "";
  int br, bc, er, ec; selOrdered(selAr_, selAc_, selCr_, selCc_, br, bc, er, ec);
  br = std::max(0, std::min(br, gridRows_ - 1));
  er = std::max(0, std::min(er, gridRows_ - 1));
  std::string out;
  for (int r = br; r <= er; r++) {
    int cs = (r == br) ? bc : 0;
    int ce = (r == er) ? ec : gridCols_ - 1;
    cs = std::max(0, std::min(cs, gridCols_ - 1));
    ce = std::max(0, std::min(ce, gridCols_ - 1));
    std::string line;
    for (int c = cs; c <= ce; c++) {
      const std::string& g = cells_[r * gridCols_ + c];
      if (g.empty()) continue;           // 継続セル
      line += g;
    }
    while (!line.empty() && line.back() == ' ') line.pop_back(); // 行末空白を落とす
    if (r != br) out += "\n";
    out += line;
  }
  return out;
}

bool Terminal::copySelection() {
  std::string text = selectedText();
  if (text.empty()) return false;
  // 1) ローカル: pbcopy へパイプ
  if (FILE* p = popen("/usr/bin/pbcopy", "w")) {
    fwrite(text.data(), 1, text.size(), p);
    pclose(p);
  }
  // 2) SSH/tmux 越しでも効くように OSC 52 も送る
  put("\x1b]52;c;" + base64(text) + "\x07");
  flush();
  return true;
}

} // namespace ta2
