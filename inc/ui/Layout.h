#pragma once

// ブレイク画面のレイアウト。画面を「逆アセンブル・レジスタ・バックトレース・スレッド」の
// 4 領域に分け、各表示を自領域内だけに描く（はみ出しは切り、余りは空白で消す）。
#include <string>

namespace ta2 {
namespace layout {

struct PaneRect {
  bool visible = false;
  int col = 0, row = 0, width = 0, height = 0;
};

struct BreakLayout {
  PaneRect disassemble;
  PaneRect registers;
  PaneRect backtrace;
  PaneRect threads;
};

const int COUNT = 5;                 // レイアウト数
const int REGISTER_ITEM_WIDTH = 27;  // レジスタ 1 項目の幅
const int REGISTER_ITEM_COUNT = 25;  // だいたいの項目数（複数列化の目安）
const int REGISTER_COLUMN_GAP = 2;

const char* name(int index);
BreakLayout compute(int index, int cols, int rows,
                    bool showRegisters, bool showBacktrace, bool showThreads);

int displayWidth(const std::string& text);              // 表示幅（ANSI=0, 全角=2）
std::string fitWidth(const std::string& text, int width); // 幅ちょうどに（長ければ … で切り、短ければ空白埋め）
std::string dropLeft(const std::string& text, int cols);  // 先頭から cols 表示桁を落とす（横スクロール用）

} // namespace layout
} // namespace ta2
