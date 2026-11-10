#include "ui/Layout.h"

#include <algorithm>
#include <cstdint>

namespace ta2 {
namespace layout {
namespace {

const int GAP = 2;
const int THREADS_WIDTH = 14;

int clampInt(int v, int lo, int hi) { return (hi < lo) ? lo : std::max(lo, std::min(v, hi)); }

int registersWidthFor(int height) {
  int rowsPerCol = std::max(1, height - 1);
  int cols = (REGISTER_ITEM_COUNT + rowsPerCol - 1) / rowsPerCol;
  return cols * REGISTER_ITEM_WIDTH + (cols - 1) * REGISTER_COLUMN_GAP;
}
int registersHeightFor(int width) {
  int cols = std::max(1, (width + REGISTER_COLUMN_GAP) / (REGISTER_ITEM_WIDTH + REGISTER_COLUMN_GAP));
  return (REGISTER_ITEM_COUNT + cols - 1) / cols + 1;
}
PaneRect rect(int col, int row, int w, int h) {
  PaneRect r; r.visible = (w > 0 && h > 0); r.col = col; r.row = row; r.width = std::max(0, w); r.height = std::max(0, h);
  return r;
}
void stackBtTh(BreakLayout& L, int col, int row, int w, int h, bool bt, bool th) {
  if (h <= 0) return;
  if (bt && th) {
    int bh = std::max(2, h * 3 / 5);
    L.backtrace = rect(col, row, w, bh);
    L.threads = rect(col, row + bh + 1, w, h - bh - 1);
  } else if (bt) L.backtrace = rect(col, row, w, h);
  else if (th) L.threads = rect(col, row, w, h);
}

} // namespace

const char* name(int index) {
  static const char* N[COUNT] = {"標準", "右サイド", "下段", "左右半分", "逆アセンブルのみ"};
  return N[((index % COUNT) + COUNT) % COUNT];
}

BreakLayout compute(int index, int cols, int rows, bool showReg, bool showBt, bool showTh) {
  BreakLayout L;
  int W = std::max(20, cols);
  int H = std::max(4, rows - 2); // 行0:ヘッダ, 最下行:コマンド
  index = ((index % COUNT) + COUNT) % COUNT;
  bool anyBottom = showBt || showTh;
  bool anySide = showReg || anyBottom;
  const int TOP = 1; // ヘッダの下から

  switch (index) {
    case 0: { // 標準: 左=逆アセ, 右=レジスタ / 下段=bt|th
      int bh = anyBottom ? clampInt(H / 3, 6, 14) : 0;
      bh = std::min(bh, H - 4);
      int th = H - bh - (anyBottom ? 1 : 0);
      int dw = W;
      if (showReg) { int rw = std::min(registersWidthFor(th), W / 2); L.registers = rect(W - rw, TOP, rw, th); dw = W - rw - GAP; }
      L.disassemble = rect(0, TOP, dw, th);
      if (anyBottom) {
        int row = TOP + th + 1;
        if (showBt) L.backtrace = rect(0, row, showTh ? W - THREADS_WIDTH - GAP : W, bh);
        if (showTh) L.threads = rect(showBt ? W - THREADS_WIDTH : 0, row, showBt ? THREADS_WIDTH : W, bh);
      }
      break;
    }
    case 1: { // 右サイド: 左=逆アセ(全高), 右列に縦積み
      int sw = anySide ? clampInt(W * 2 / 5, REGISTER_ITEM_WIDTH, 90) : 0;
      L.disassemble = rect(0, TOP, W - (anySide ? sw + GAP : 0), H);
      int col = W - sw, row = TOP;
      if (showReg) { int rh = std::min(registersHeightFor(sw), anyBottom ? H - 6 : H); L.registers = rect(col, row, sw, rh); row += rh + 1; }
      stackBtTh(L, col, row, sw, TOP + H - row, showBt, showTh);
      break;
    }
    case 2: { // 下段: 上=逆アセ(全幅), 下段に横並び
      int bh = anySide ? clampInt(H * 2 / 5, 8, 16) : 0;
      bh = std::min(bh, H - 4);
      int th = H - bh - (anySide ? 1 : 0);
      L.disassemble = rect(0, TOP, W, th);
      if (anySide) {
        int row = TOP + th + 1, col = 0;
        if (showReg) { int rw = std::min(registersWidthFor(bh), anyBottom ? W / 2 : W); L.registers = rect(col, row, rw, bh); col += rw + GAP; }
        int thCol = W - THREADS_WIDTH;
        if (showBt) L.backtrace = rect(col, row, (showTh ? thCol - GAP : W) - col, bh);
        if (showTh) L.threads = rect(showBt ? thCol : col, row, showBt ? THREADS_WIDTH : W - col, bh);
      }
      break;
    }
    case 3: { // 左右半分
      int lw = anySide ? (W - GAP) / 2 : W;
      L.disassemble = rect(0, TOP, lw, H);
      int col = lw + GAP, sw = W - col, row = TOP;
      if (showReg) { int rh = std::min(registersHeightFor(sw), anyBottom ? H - 6 : H); L.registers = rect(col, row, sw, rh); row += rh + 1; }
      stackBtTh(L, col, row, sw, TOP + H - row, showBt, showTh);
      break;
    }
    default: // 逆アセンブルのみ
      L.disassemble = rect(0, TOP, W, H);
      break;
  }
  return L;
}

// ---- 表示幅ユーティリティ（UTF-8 / ANSI 対応） ----
static size_t utf8Len(unsigned char c) {
  if (c < 0x80) return 1; if ((c & 0xE0) == 0xC0) return 2; if ((c & 0xF0) == 0xE0) return 3; if ((c & 0xF8) == 0xF0) return 4; return 1;
}
static int charWidth(const std::string& t, size_t pos, size_t len) {
  if (len < 3) return 1;
  uint32_t cp = 0;
  if (len == 3) cp = ((t[pos] & 0x0F) << 12) | ((t[pos+1] & 0x3F) << 6) | (t[pos+2] & 0x3F);
  else cp = ((t[pos] & 0x07) << 18) | ((t[pos+1] & 0x3F) << 12) | ((t[pos+2] & 0x3F) << 6) | (t[pos+3] & 0x3F);
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD)) return 2;
  return 1;
}
static size_t escLen(const std::string& t, size_t pos) {
  if (t[pos] != '\x1b' || pos + 1 >= t.size() || t[pos+1] != '[') return 0;
  size_t e = pos + 2;
  while (e < t.size() && !((t[e] >= 'A' && t[e] <= 'Z') || (t[e] >= 'a' && t[e] <= 'z'))) e++;
  return (e < t.size()) ? (e - pos + 1) : (t.size() - pos);
}

int displayWidth(const std::string& t) {
  int w = 0; size_t pos = 0;
  while (pos < t.size()) {
    size_t e = escLen(t, pos); if (e) { pos += e; continue; }
    size_t len = std::min(utf8Len((unsigned char)t[pos]), t.size() - pos);
    w += charWidth(t, pos, len); pos += len;
  }
  return w;
}

std::string fitWidth(const std::string& t, int width) {
  if (width <= 0) return std::string();
  int total = displayWidth(t);
  if (total <= width) return t + std::string(width - total, ' ');
  std::string r; int used = 0; bool styled = false; size_t pos = 0;
  while (pos < t.size()) {
    size_t e = escLen(t, pos); if (e) { r += t.substr(pos, e); styled = true; pos += e; continue; }
    size_t len = std::min(utf8Len((unsigned char)t[pos]), t.size() - pos);
    int w = charWidth(t, pos, len);
    if (used + w > width - 1) break;
    r += t.substr(pos, len); used += w; pos += len;
  }
  r += "…"; used += 1;
  if (styled) r += "\x1b[0m";
  if (used < width) r += std::string(width - used, ' ');
  return r;
}

std::string dropLeft(const std::string& t, int cols) {
  if (cols <= 0) return t;
  int dropped = 0; size_t pos = 0;
  while (pos < t.size() && dropped < cols) {
    size_t e = escLen(t, pos); if (e) { pos += e; continue; } // エスケープは桁を持たない
    size_t len = std::min(utf8Len((unsigned char)t[pos]), t.size() - pos);
    dropped += charWidth(t, pos, len);
    pos += len;
  }
  std::string r = t.substr(pos);
  // 全角を途中で切ったぶんは空白で埋めて桁をそろえる
  if (dropped > cols) r = std::string(dropped - cols, ' ') + r;
  return r;
}

} // namespace layout
} // namespace ta2
