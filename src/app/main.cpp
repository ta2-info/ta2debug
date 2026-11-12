// ta2debug の対話 TUI（最小構成）。
//   起動 → :attach <pid> → :b <hexaddr> → ブレイクで停止 → s / n / c でステップ・継続 → :q
// 入力はスレッドで読み、例外（停止）は Session から EventQueue 経由でメインに届く。
// 描画はメインスレッドだけが行う。アーキ依存は Arch に閉じているので arm64 でも同じ。
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "app/EventQueue.h"
#include "app/Expression.h"
#include "core/Session.h"
#include "core/Disassembler.h"
#include "core/ProcessList.h"
#include "core/Unwind.h"
#include "ui/Terminal.h"
#include "ui/Layout.h"

using namespace ta2;

namespace {

Terminal g_term;
EventQueue g_queue;
Session g_session(g_queue);

enum class UiState { Idle, Running, Stopped };
UiState g_state = UiState::Idle;

std::string g_cmdline;
bool g_cmdMode = false;
std::string g_message; // 最下行の通知

bool g_interactive = true; // TUI か（false=バッチ。イベントループが無いのでジョブは同期実行）

// ---- 非同期シンボル式評価ジョブ ----
// b/bh/w/sym などのシンボル式評価は、既定ライブラリ／主実行ファイルに無い名前だと
// 全ロード済みイメージを横断して探す。cold では各イメージの symtab 読込が重く、
// メインスレッドで同期実行すると UI が固まる。そこで別スレッドで評価し、メインは
// イベントループに留まって再描画・入力を続ける。重い探索に入ったときだけ進捗を出し、
// ESC で中止できる。速い評価（主/既定で見つかる）は進捗を出さずそのまま完了する。
std::thread g_jobThread;
std::atomic<bool> g_jobRunning{false};  // ワーカ走行中（完了イベント処理まで true）
std::atomic<bool> g_jobCancel{false};   // ESC による中止要求
std::atomic<bool> g_jobShowing{false};  // 進捗表示を出す段階か（＝重い全画像探索に入った）
std::atomic<int> g_jobDone{0};          // 探索済みイメージ数
std::atomic<int> g_jobTotal{0};         // 探索対象イメージ総数
// 評価結果（JobDone をメインで処理する時のみ参照。ワーカ完了後に読むので同期は不要）。
bool g_jobOk = false; uint64_t g_jobAddr = 0; std::string g_jobErr;
std::function<void(bool, uint64_t, const std::string&)> g_jobApply; // 結果適用（メインで実行）

// 参照コマンド（reg/x/img など）の複数行出力オーバレイ。空でなければ本体の上に重ねて表示する。
// ページャ風にスクロール／ページ送りでき、ESC・q で閉じる。
std::vector<std::string> g_output;
std::string g_outputTitle;
int g_outputTop = 0;           // オーバレイのスクロール先頭行（新しい出力で 0 に戻す）
int g_ovEsc = 0;               // オーバレイ内のエスケープ列: 0=なし,1=ESC,2=ESC[

// :x のアドレス継続。直近の examine を覚えておき、オーバレイ表示中の Enter で続きを表示する。
bool g_xActive = false;        // いま表示中のオーバレイが :x の結果か（Enter で継続可）
bool g_xHasLast = false;       // 一度でも :x を実行したか（引数なし :x で続きを表示）
std::string g_xSpec;           // 書式指定（空なら従来の 16 進ダンプ）
int g_xClassicBytes = 64;      // 従来ダンプのバイト数
uint64_t g_xNextAddr = 0;      // 次に表示する開始アドレス

// auto（興味のある命令まで自動継続）。g_autoMask のビット: 1=call, 2=syscall, 4=ret。0 で無効。
// 停止のたびに、現命令が該当すれば止め、そうでなければ自動で単一ステップを続ける。
int g_autoMask = 0;
long g_autoSteps = 0;              // 今回の auto で踏んだステップ数
const long kAutoMax = 2000000;     // 暴走防止の上限

// 停止位置の命令種別（auto 判定用）。ブレイク命令バイトは元に戻して判定。
InsnKind kindAtPc(uint64_t pc) {
  uint8_t code[16] = {}; size_t got = 0;
  if (!g_session.target().readMemory(pc, code, sizeof(code), &got) || !got) return InsnKind::Other;
  g_session.maskBreakpoints(pc, code, got);
  Disassembler dis(g_session.arch());
  auto in = dis.disasm(code, got, pc, 1);
  return in.empty() ? InsnKind::Other : in[0].kind;
}

// レイアウト設定（l/L で切替、1/2/3 で表示切替、0 で全表示）
int g_layout = 0;
bool g_showReg = true, g_showBt = true, g_showTh = true;

// ブレイク中のナビゲーション（TUI のみ。バッチは使わない）。
// フォーカス: 逆アセンブル（↑↓ でコードをスクロール）か スレッド選択（↑↓ でスレッド切替）。Tab で切替。
enum class Focus { Disasm, Threads };
Focus g_focus = Focus::Disasm;
int g_mainEsc = 0;             // メイン画面のエスケープ列: 0=なし,1=ESC,2=ESC[
uint64_t g_disasmTop = 0;      // 逆アセンブル表示の先頭アドレス（追従解除時に有効）
bool g_disasmFollow = true;    // true=常に現在 PC を先頭に追従
std::vector<uint64_t> g_disasmTrail; // 前方スクロールで通過した先頭アドレス履歴（後方で正確に戻す）
int g_disasmHeight = 16;       // 直近の逆アセンブル領域の行数（ページ送り量に使う）
bool g_baseRelative = false;   // 逆アセンブルのアドレスを「イメージ先頭からの相対値」で表示（既定=実アドレス）
int g_threadSel = 0;           // スレッド一覧の選択位置
thread_act_t g_viewThread = 0; // 表示中スレッド（0=停止スレッド）。他スレッドのレジスタ/逆アセンブルを見る用
std::vector<uint8_t> g_viewBuf; // 非停止スレッド表示用の一時スレッド状態

// プロセス選択（:attach を pid 無しで実行したとき）
bool g_pickerMode = false;
std::vector<ProcessInfo> g_procs;
std::string g_pickFilter;
int g_pickSel = 0;      // 絞り込み後リストの選択位置
int g_pickTop = 0;      // スクロール先頭（絞り込み後インデックス）
int g_pickHScroll = 0;  // 選択行の横スクロール量（表示桁。選択移動・フィルタ変更で 0 に戻す）
int g_escState = 0;     // ピッカー内のエスケープ列: 0=なし, 1=ESC, 2=ESC[

uint64_t parseNum(const std::string& s) { return strtoull(s.c_str(), nullptr, 0); }

using layout::PaneRect;

// 領域に見出し＋行を描く（はみ出す行は "…ほか N 行"、余りは空白）
void drawPane(const PaneRect& p, const std::string& title, const std::vector<std::string>& lines) {
  if (!p.visible) return;
  int cap = p.height - 1;
  bool overflow = (int)lines.size() > cap && cap > 0;
  for (int y = 0; y < p.height; y++) {
    std::string text;
    if (y == 0) text = "\x1b[33m" + title + "\x1b[0m"; // 見出し（黄）
    else if (overflow && y == p.height - 1)
      text = "\x1b[2m…ほか " + std::to_string((int)lines.size() - (cap - 1)) + " 行\x1b[0m";
    else if (y - 1 < (int)lines.size()) text = lines[y - 1];
    g_term.putAt(p.col, p.row + y, layout::fitWidth(text, p.width));
  }
}

// 逆アセンブル領域（関数ラベル・現在行反転・call 先名）
// 表示中スレッド（g_viewThread、0=停止スレッド）のレジスタ状態。バッチ・未選択時は停止スレッド。
const uint8_t* viewStateData() {
  if (!g_viewThread || g_viewThread == g_session.stopThread()) return g_session.stopState().data();
  g_viewBuf.assign(g_session.arch().threadStateCount() * sizeof(natural_t), 0);
  if (!g_session.target().getThreadState(g_viewThread, g_viewBuf.data())) return g_session.stopState().data();
  return g_viewBuf.data();
}
uint64_t viewPc() { return g_session.arch().pc(viewStateData()); }
bool viewingStopThread() { return !g_viewThread || g_viewThread == g_session.stopThread(); }

// top の直前の命令の先頭アドレス（x86 可変長のための後方探索）。見つからなければ top-1。
// top の n 命令前のアドレスを返す。x86 は可変長で後方デコードが曖昧なので、
// 手前から前方デコードして top にちょうど揃う命令列を探し、その n 個前を返す
// （前方の実境界を再現するので「前方へ n → 後方へ n」が元に戻る）。見つからなければ top。
uint64_t disasmBack(uint64_t top, int n) {
  if (n <= 0) return top;
  Disassembler dis(g_session.arch());
  const size_t kMaxLen = 15;                 // x86 命令の最大長
  size_t window = (size_t)n * kMaxLen + 32;  // n 命令を確実に含む手前幅
  for (size_t slack = 0; slack <= 24; slack++) { // 開始位置を 1 byte ずつずらし top に同期する並びを探す
    uint64_t start = top - (uint64_t)(window + slack);
    size_t len = window + slack + kMaxLen; std::vector<uint8_t> buf(len, 0); size_t got = 0;
    if (!g_session.target().readMemory(start, buf.data(), len, &got) || got < window) continue;
    g_session.maskBreakpoints(start, buf.data(), got);
    auto insns = dis.disasm(buf.data(), got, start, 4096);
    int topIdx = -1;
    for (int i = 0; i < (int)insns.size(); i++) {
      if (insns[i].address == top) { topIdx = i; break; }  // top が境界として現れた＝同期した
      if (insns[i].address > top) break;                   // top を跨いだ＝ずれている。slack を変えて再試行
    }
    if (topIdx >= 0) return insns[std::max(0, topIdx - n)].address;
  }
  return top; // 同期できない異常時は動かさない
}

void drawDisassembly(const PaneRect& p) {
  if (!p.visible) return;
  g_disasmHeight = p.height;                               // ページ送り量に使う
  Arch& a = g_session.arch();
  uint64_t pc = viewPc();                                  // 表示中スレッドの現在命令（反転表示の対象）
  uint64_t start = g_disasmFollow ? pc : g_disasmTop;      // 表示の先頭（スクロール中は追従解除）
  uint8_t code[512] = {}; size_t got = 0;
  g_session.target().readMemory(start, code, sizeof(code), &got);
  g_session.maskBreakpoints(start, code, got);
  Disassembler dis(a);
  const Image* img = g_session.images().byAddress(start);
  auto insns = dis.disasm(code, got, start, p.height);
  int y = 0;
  uint64_t lastFunc = 0; bool haveLast = false;
  std::string lastSrcFile; int lastSrcLine = -1;
  for (size_t i = 0; i < insns.size() && y < p.height; i++) {
    const Symbol* fn = img ? g_session.symbols().byAddress(*img, insns[i].address) : nullptr;
    if (fn && (!haveLast || lastFunc != fn->addr)) {
      g_term.putAt(p.col, p.row + y++, layout::fitWidth("\x1b[36m" + g_session.symbols().displayName(*fn) + ":\x1b[0m", p.width));
      if (y >= p.height) break;
    }
    haveLast = fn != nullptr; lastFunc = fn ? fn->addr : 0;
    // ソース行（dSYM の DWARF 行情報）。行が変わった所に淡色で挿入（lldb 風）。
    SourceLoc loc;
    if (g_session.sourceLineFor(insns[i].address, loc) && (loc.line != lastSrcLine || loc.file != lastSrcFile)) {
      lastSrcFile = loc.file; lastSrcLine = loc.line;
      std::string head = loc.file + ":" + std::to_string(loc.line);
      std::string t = loc.text; size_t a0 = t.find_first_not_of(" \t"); if (a0 != std::string::npos) t = t.substr(a0); else t.clear();
      std::string sl = head + (t.empty() ? "" : ("  " + t));
      g_term.putAt(p.col, p.row + y++, layout::fitWidth("\x1b[2m" + sl + "\x1b[0m", p.width));
      if (y >= p.height) break;
    }
    char off[24] = "";
    if (fn) snprintf(off, sizeof(off), "+0x%llx", (unsigned long long)(insns[i].address - fn->addr));
    // baseoff 時はイメージ先頭からの相対値で表示（既定は実アドレス）。
    uint64_t dispAddr = (g_baseRelative && img) ? (insns[i].address - img->loadAddr) : insns[i].address;
    char line[256];
    // 先頭 2 桁はブレイク印の溝（SW: ●有効/○無効、HW: ◆有効/◇無効）。アドレスはその後ろから。
    snprintf(line, sizeof(line), "  0x%010llx %-7s %-7s %s", (unsigned long long)dispAddr,
             off, insns[i].mnemonic.c_str(), insns[i].opStr.c_str());
    std::string s(line);
    if (insns[i].hasCallTarget) { std::string t = g_session.symbolName(insns[i].callTarget); if (!t.empty()) s += "  ; " + t; }
    bool cur = insns[i].address == pc;
    if (cur && viewingStopThread() && insns[i].kind == InsnKind::Syscall) { // これから呼ぶシステムコールを注釈
      std::string sc = g_session.syscallAtStop();
      if (!sc.empty()) s += "  ; → " + sc;
    }
    s = layout::fitWidth(s, p.width);
    if (cur) s = "\x1b[7m" + s + "\x1b[0m";
    int lineRow = p.row + y++;
    g_term.putAt(p.col, lineRow, s);
    const Breakpoint* ub = g_session.breaks().userAt(insns[i].address);
    if (ub) {
      const char* mark = ub->hw ? (ub->enabled ? "\x1b[31m◆\x1b[0m" : "\x1b[2m◇\x1b[0m")
                                : (ub->enabled ? "\x1b[31m●\x1b[0m" : "\x1b[2m○\x1b[0m");
      g_term.putAt(p.col, lineRow, mark);
    }
  }
  for (; y < p.height; y++) g_term.putAt(p.col, p.row + y, std::string(p.width, ' '));
}

// レジスタ領域（幅に応じて複数列）
void drawRegisters(const PaneRect& p) {
  if (!p.visible) return;
  auto regs = g_session.arch().registers(viewStateData());
  std::vector<std::string> items;
  for (auto& r : regs) {
    char b[64]; snprintf(b, sizeof(b), "%6s = 0x%016llx", r.name.c_str(), (unsigned long long)r.value);
    items.push_back(b);
  }
  int rowsAvail = std::max(1, p.height - 1);
  int colsFit = std::max(1, (p.width + layout::REGISTER_COLUMN_GAP) / (layout::REGISTER_ITEM_WIDTH + layout::REGISTER_COLUMN_GAP));
  int colsNeed = ((int)items.size() + rowsAvail - 1) / rowsAvail;
  int columns = std::min(colsFit, std::max(1, colsNeed));
  int perCol = ((int)items.size() + columns - 1) / columns;
  std::vector<std::string> lines;
  for (int y = 0; y < perCol; y++) {
    std::string line;
    for (int c = 0; c < columns; c++) {
      size_t idx = (size_t)(c * perCol + y);
      if (idx >= items.size()) break;
      line += (c ? std::string(layout::REGISTER_COLUMN_GAP, ' ') : "") + layout::fitWidth(items[idx], layout::REGISTER_ITEM_WIDTH);
    }
    lines.push_back(line);
  }
  drawPane(p, "レジスタ", lines);
}

// バックトレース領域（フレームポインタ鎖。[fp]=saved fp, [fp+8]=return。両アーキ共通）
// アドレスがいずれかのイメージの実行領域（__TEXT）内か。戻りアドレスの妥当性判定に使う。
static bool addrIsCode(uint64_t x) {
  if (!x) return false;
  for (const auto& im : g_session.images().images()) if (im.containsText(x)) return true;
  return false;
}

// x が「call/bl の直後（＝正当な戻りアドレス）」らしいか。
// x の直前に、ちょうど x で終わる call 命令が置かれているかを逆アセンブルで確かめる。
// スタック走査での誤検出（たまたまコードを指すデータ）を弾くための強い判定。
static bool addrIsCallReturn(Disassembler& dis, uint64_t x) {
  if (!addrIsCode(x)) return false;
  for (int len = 2; len <= 8; len++) {          // x86 call は 2〜7B / arm64 bl は 4B
    uint64_t s = x - (uint64_t)len;
    if (!addrIsCode(s)) continue;
    uint8_t buf[8] = {};
    if (!g_session.target().readMemory(s, buf, (size_t)len)) continue;
    auto in = dis.disasm(buf, (size_t)len, s, 1);
    if (!in.empty() && in[0].size == len && in[0].kind == InsnKind::Call) return true;
  }
  return false;
}

// from 以上のスタックを上へ走査し、最初の「戻りアドレスらしい」語を返す（見つけた語のアドレスも）。
static bool scanStackForReturn(Disassembler& dis, uint64_t from, uint64_t& outRet, uint64_t& outSlot) {
  uint64_t base = (from + 7) & ~(uint64_t)7; // 8 境界
  uint8_t buf[8192];
  size_t got = 0;
  for (size_t win = sizeof(buf); win >= 256; win /= 2) // 末尾ガードページ手前は小さく再試行
    if (g_session.target().readMemory(base, buf, win, &got)) { got = win; break; }
  if (!got) return false;
  for (size_t off = 0; off + 8 <= got; off += 8) {
    uint64_t v = 0; memcpy(&v, buf + off, 8);
    if (addrIsCallReturn(dis, v)) { outRet = v; outSlot = base + off; return true; }
  }
  return false;
}

// バックトレースを計算し、1 フレーム 1 行の配列で返す（TUI 描画・バッチ出力の共通土台）。
// color=true なら復元の注記に ANSI 淡色を付ける（TUI 用）。false なら素のテキスト（バッチ用）。
// fast=true のときは高コストなスタック走査フォールバックを使わない（CFI/fp が切れたら打ち切る）。
// 停止スレッド以外（スレッド選択で覗く別スレッド）は走査が重くなりがちなので fast で描画を軽くする。
std::vector<std::string> computeBacktrace(bool color, bool fast = false) {
  auto note = [color](const char* s) {
    return color ? (std::string("  \x1b[2m") + s + "\x1b[0m") : (std::string("  ") + s);
  };
  std::vector<std::string> lines;
  Arch& a = g_session.arch();
  const uint8_t* st = viewStateData(); // 表示中スレッド（バッチ・未選択なら停止スレッド）
  Disassembler dis(a);
  MemReader reader = [](uint64_t addr, void* buf, size_t len) {
    return g_session.target().readMemory(addr, buf, len);
  };
  Unwinder unw(g_session.images(), a, reader);

  const Image* mainExe = g_session.images().mainExecutable();
  UnwindRegs regs;
  regs.pc = a.pc(st); // 表示中スレッドの PC（停止スレッドなら stopPc と一致）
  regs.sp = a.sp(st);
  regs.fp = a.fp(st);
  regs.lr = a.linkRegister(st); // arm64 の leaf frameless 用（x86 は 0）
  uint64_t scanBase = regs.sp;  // スタック走査の開始点
  bool scanning = false;        // いちど連鎖が壊れたら以降は走査で辿る
  bool sawMain = false;
  std::string pendingNote;

  for (int depth = 0; depth < 64; depth++) {
    char head[32]; snprintf(head, sizeof(head), "0x%012llx ", (unsigned long long)regs.pc);
    std::string w = g_session.describe(regs.pc);
    lines.push_back(std::string(head) + (w.empty() ? "" : (" " + w)) + pendingNote);
    pendingNote.clear();

    const Image* im = g_session.images().byAddress(regs.pc);
    if (im && im == mainExe) sawMain = true;
    // メインを通過して dyld（start）まで達したら実質の最外フレーム。この 1 枚を出して打ち切る
    //（その上はローダのブート領域で、辿っても残骸＝偽フレームになるだけ）。
    if (sawMain && im && im != mainExe && im->name.find("dyld") != std::string::npos) break;

    // (1) まだ自分のフレームを作っていない位置（プロローグ未実行）で止まっている場合。
    //     この時点では rbp/x29 は呼び出し元のままなので、CFI/fp の frame ルールを当てると
    //     直近の呼び出し元を飛ばす。戻り先を [sp](x86)/LR(arm64) から 1 段復元する。該当するのは:
    //       (a) IP=0 等、どのイメージにも属さない異常ジャンプ
    //       (b) 関数の最初の命令（:b foo で foo+0 停止、call した直後）
    //       (c) unwind 情報の無いコード（dylib 遅延スタブ等の `jmp *ptr` サンク。フレームを作らない）
    if (depth == 0) {
      uint32_t enc = 0; uint64_t fstart = 0;
      bool hasEnc = addrIsCode(regs.pc) && unw.encodingFor(regs.pc, enc, fstart);
      bool certain = !addrIsCode(regs.pc) || (hasEnc && regs.pc == fstart); // (a)(b): [sp] は確実に戻り先
      bool stub = addrIsCode(regs.pc) && !hasEnc;                           // (c): スタブ/サンク疑い
      if (certain || stub) {
        uint64_t ret = 0;
        if (a.retAddrAtEntryOnStack()) {
          uint64_t top = 0;
          if (g_session.target().readMemory(regs.sp, &top, sizeof(top))) ret = top;
        } else ret = regs.lr;
        // (c) は確証が無いので「call/bl 直後らしさ」で誤検出を弾く。(a)(b) は addrIsCode で足りる。
        bool ok = certain ? addrIsCode(ret) : addrIsCallReturn(dis, ret);
        if (ok) {
          if (a.retAddrAtEntryOnStack()) regs.sp += 8; // 戻り先をポップした後の sp
          regs.pc = ret;                               // fp は据え置き（＝呼び出し元フレーム）
          pendingNote = a.retAddrAtEntryOnStack() ? note("← [sp] から復元")
                                                  : note("← lr から復元");
          scanBase = regs.sp;
          continue;
        }
      }
    }

    // (2) 本命: CFI（compact unwind）で正確に 1 段。RBP フレームも frameless も正しく辿れる。
    if (!scanning && addrIsCode(regs.pc)) {
      UnwindRegs next;
      if (unw.step(regs, next) && next.pc && addrIsCode(next.pc) && next.sp > regs.sp) {
        scanBase = regs.fp ? regs.fp + 16 : next.sp;
        regs = next;
        continue;
      }
    }

    // (3) フォールバック: __unwind_info が無い/DWARF 等。フレームポインタ(rbp/x29)連鎖で次段。
    if (!scanning && regs.fp) {
      uint64_t slot[2] = {};
      if (g_session.target().readMemory(regs.fp, slot, sizeof(slot)) &&
          slot[0] > regs.fp && addrIsCode(slot[1])) {
        scanBase = regs.fp + 16;
        regs.sp = regs.fp + 16; regs.pc = slot[1]; regs.fp = slot[0];
        pendingNote = note("← fp 連鎖");
        continue;
      }
    }

    // (4) 特別対応: 連鎖が切れた／呼び出し元が不正。スタック走査で戻りアドレスを復元（推定）。
    //     常に sp から上へ（fp は壊れている可能性があるので当てにしない）。
    if (fast) break; // 別スレッド表示時は高コストな走査を避け、ここで打ち切る
    uint64_t ret = 0, slotAt = 0;
    uint64_t from = scanning ? scanBase : regs.sp;
    if (scanStackForReturn(dis, from, ret, slotAt)) {
      regs.pc = ret; regs.fp = 0; regs.sp = slotAt + 8;
      scanning = true; scanBase = slotAt + 8;
      pendingNote = note("← スタック走査で復元(推定)");
      continue;
    }
    break; // これ以上は辿れない
  }
  return lines;
}

void drawBacktrace(const PaneRect& p) {
  if (!p.visible) return;
  // 停止スレッドは従来どおり完全なバックトレース。別スレッドを覗くときは走査を省いて軽量に。
  drawPane(p, "バックトレース", computeBacktrace(true, !viewingStopThread()));
}

// スレッド領域（★=停止スレッド / 👁=表示中。Tab でフォーカスすると ↑↓ で選択＝表示切替）
void drawThreads(const PaneRect& p) {
  if (!p.visible) return;
  std::vector<std::string> lines;
  auto ths = g_session.target().threads();
  std::sort(ths.begin(), ths.end()); // selectThread と同じ安定順
  thread_act_t stopT = g_session.stopThread();
  thread_act_t viewT = g_viewThread ? g_viewThread : stopT;
  bool focus = (g_focus == Focus::Threads);
  for (thread_act_t t : ths) {
    const char* m = (t == stopT) ? "★" : "  ";
    char b[48]; snprintf(b, sizeof(b), "%s%s 0x%x", m, (t == viewT ? "👁" : "  "), t);
    std::string s(b);
    if (focus && t == viewT) s = "\x1b[7m" + s + "\x1b[0m"; // 表示中スレッドの行を反転（tid 基準）
    lines.push_back(s);
  }
  drawPane(p, std::string(focus ? "▶" : "") + "スレッド " + std::to_string(ths.size()), lines);
}

// ----- プロセス選択 -----
void doAttach(pid_t pid) {
  std::string err;
  if (g_session.attach(pid, err)) {
    g_state = UiState::Running;
    g_message = "attached pid " + std::to_string(pid);
  } else {
    g_message = "attach 失敗: " + err;
  }
}

// バイナリを起動してデバッグ開始。args[0]=実行ファイルパス、以降は対象への引数。
void doLaunch(const std::vector<std::string>& args) {
  if (args.empty() || args[0].empty()) { g_message = "起動するファイルパスを指定してください"; return; }
  std::vector<char*> cargv;
  for (const auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);
  std::string err;
  if (g_session.launch(args[0], cargv.data(), err)) {
    g_state = g_session.stopped() ? UiState::Stopped : UiState::Running; // エントリへ走行中→Running、停止済み→Stopped
    g_message = "起動: " + args[0] + "  （エントリで停止します。出力は " + args[0] + ".ta2debug.log）";
  } else {
    g_message = "起動失敗: " + err;
  }
}

void openPicker() {
  g_procs = listProcesses();
  g_pickFilter.clear();
  g_pickSel = 0;
  g_pickTop = 0;
  g_escState = 0;
  g_pickerMode = true;
}

void closePicker() {
  g_pickerMode = false;
  g_pickFilter.clear();
  g_escState = 0;
}

// 現在のフィルタに一致するプロセス。フィルタを空白区切りのトークンに分け、
// 「名前 パス 引数 pid」をつないだ文字列に全トークンが含まれる行だけ残す（大小無視・順不同）。
std::vector<const ProcessInfo*> pickerFiltered() {
  std::vector<const ProcessInfo*> r;

  std::vector<std::string> toks;
  {
    std::string cur;
    for (char c : g_pickFilter) {
      if (c == ' ' || c == '\t') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
      else cur += (char)std::tolower((unsigned char)c);
    }
    if (!cur.empty()) toks.push_back(cur);
  }

  for (const auto& p : g_procs) {
    std::string hay = p.name + " " + p.path + " " + p.args + " " + std::to_string(p.pid);
    std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    bool all = true;
    for (const auto& t : toks) if (hay.find(t) == std::string::npos) { all = false; break; }
    if (all) r.push_back(&p);
  }
  return r;
}

// 1 行ぶんの固定プレフィックス（マーカ＋pid）と、横スクロール対象の本体（名前＋引数）。
std::string pickerPrefix(const ProcessInfo* p) {
  char b[32]; snprintf(b, sizeof(b), " %s %6d  ", p->attachable ? "✓" : "×", p->pid);
  return std::string(b);
}
std::string pickerBody(const ProcessInfo* p) {
  std::string disp = !p->name.empty() ? p->name : (!p->path.empty() ? p->path : std::string("(不明)"));
  if (!p->args.empty()) disp += "  " + p->args;
  return disp;
}

void pickerMove(int delta) {
  auto f = pickerFiltered();
  g_pickHScroll = 0; // 選択が動いたら横位置は先頭へ
  if (f.empty()) { g_pickSel = 0; return; }
  g_pickSel += delta;
  if (g_pickSel < 0) g_pickSel = 0;
  if (g_pickSel >= (int)f.size()) g_pickSel = (int)f.size() - 1;
}

// 選択行の横スクロール（dir=+1 右 / -1 左）。本体がウィンドウに収まる行では何もしない。
void pickerScrollH(int dir) {
  int step = std::max(1, g_term.cols() / 2); // 画面幅の半分ずつ送る
  g_pickHScroll += dir * step;
  if (g_pickHScroll < 0) g_pickHScroll = 0;
  auto f = pickerFiltered();
  if (g_pickSel >= 0 && g_pickSel < (int)f.size()) {
    const ProcessInfo* p = f[g_pickSel];
    int bodyW = layout::displayWidth(pickerBody(p));
    int visBody = std::max(1, g_term.cols() - layout::displayWidth(pickerPrefix(p)));
    int maxScroll = std::max(0, bodyW - visBody); // これ以上送っても末尾が見えるだけ
    if (g_pickHScroll > maxScroll) g_pickHScroll = maxScroll;
  } else {
    g_pickHScroll = 0;
  }
}

// ページ送り（dir=+1 次ページ / -1 前ページ）。選択とスクロールを 1 画面分まとめて動かす。
void pickerPage(int dir) {
  auto f = pickerFiltered();
  int n = (int)f.size();
  g_pickHScroll = 0; // 選択が動いたら横位置は先頭へ
  if (n == 0) { g_pickSel = 0; g_pickTop = 0; return; }
  int rows = std::max(1, g_term.rows() - 3); // ヘッダ + フィルタ + フッタ
  g_pickSel += dir * rows;
  if (g_pickSel < 0) g_pickSel = 0;
  if (g_pickSel >= n) g_pickSel = n - 1;
  int maxTop = std::max(0, n - rows);
  g_pickTop += dir * rows;
  if (g_pickTop < 0) g_pickTop = 0;
  if (g_pickTop > maxTop) g_pickTop = maxTop;
}

void drawPicker(int rows, int cols) {
  auto f = pickerFiltered();
  int okCount = 0;
  for (auto* p : f) if (p->attachable) okCount++;

  char hdr[160];
  snprintf(hdr, sizeof(hdr), " ta2debug  プロセス選択  表示 %d/%d 件  アタッチ可 %d 件 ",
           (int)f.size(), (int)g_procs.size(), okCount);
  g_term.putAt(0, 0, std::string("\x1b[7m") + layout::fitWidth(hdr, cols) + "\x1b[0m");
  g_term.putAt(0, 1, layout::fitWidth("フィルタ: " + g_pickFilter + "\x1b[7m \x1b[0m"
                                      + "   \x1b[2m（×印=アタッチ不可）\x1b[0m", cols));

  int listTop = 2;
  int listRows = std::max(1, rows - 3); // ヘッダ + フィルタ + フッタ

  if (g_pickSel >= (int)f.size()) g_pickSel = (int)f.size() - 1;
  if (g_pickSel < 0) g_pickSel = 0;
  if (g_pickSel < g_pickTop) g_pickTop = g_pickSel;
  if (g_pickSel >= g_pickTop + listRows) g_pickTop = g_pickSel - listRows + 1;
  if (g_pickTop < 0) g_pickTop = 0;

  for (int y = 0; y < listRows; y++) {
    int idx = g_pickTop + y;
    if (idx < (int)f.size()) {
      const ProcessInfo* p = f[idx];
      bool ok = p->attachable;
      bool sel = (idx == g_pickSel);
      std::string prefix = pickerPrefix(p);
      std::string body = pickerBody(p); // 名前＋引数（横スクロール対象）
      // 選択行だけ横スクロール。プレフィックス（マーカ＋pid）は固定。
      if (sel && g_pickHScroll > 0) body = layout::dropLeft(body, g_pickHScroll);
      std::string text = layout::fitWidth(prefix + body, cols);
      // 選択行は反転。アタッチ不可の行は淡色（選択時は反転＋淡色）。
      std::string on;
      if (sel && !ok) on = "\x1b[7;2m";
      else if (sel) on = "\x1b[7m";
      else if (!ok) on = "\x1b[2m";
      if (!on.empty()) text = on + text + "\x1b[0m";
      g_term.putAt(0, listTop + y, text);
    } else {
      g_term.putAt(0, listTop + y, std::string(cols, ' '));
    }
  }

  g_term.putAt(0, rows - 1,
               layout::fitWidth("↑↓/C-p,C-n 行   C-f/C-b ページ   ←→ 引数スクロール   Enter アタッチ   文字でフィルタ   ESC 取消", cols));
}

// ----- 描画 -----
void render() {
  g_term.querySize();
  int rows = g_term.rows(), cols = g_term.cols();
  g_term.clear();

  if (g_pickerMode) {
    drawPicker(rows, cols);
    g_term.flush();
    return;
  }

  // ヘッダ
  char hdr[256];
  const char* st = g_state == UiState::Idle ? "No attached"
                 : g_state == UiState::Running ? "running" : "stopped";
  if (g_session.isAttached())
    snprintf(hdr, sizeof(hdr), "ta2debug  pid:%d  [%s]", g_session.pid(), st);
  else
    snprintf(hdr, sizeof(hdr), "ta2debug  [%s]", st);
  std::string hline = std::string("\x1b[7m ") + hdr;
  if (g_state == UiState::Stopped) {
    std::string where = g_session.describe(g_session.stopPc());
    if (!where.empty()) hline += "  @ " + where;
    const Breakpoint* ub = g_session.breaks().userAt(g_session.stopPc());
    if (ub) { char bm[56]; snprintf(bm, sizeof(bm), "  %s#%d(%lluhit)", ub->hw ? "◆" : "●", ub->id, (unsigned long long)ub->hits); hline += bm; }
    int wid = g_session.lastWatchHit(); // ウォッチ命中（PC は命令側なので別途表示）
    if (wid >= 0) { const Breakpoint* wb = g_session.breaks().byId(wid);
      if (wb) { char bm[96]; snprintf(bm, sizeof(bm), "  ◆W#%d(%lluhit) @0x%llx", wid, (unsigned long long)wb->hits, (unsigned long long)wb->addr); hline += bm; } }
    if (g_session.stopException() != 0) hline += "  ⚠ 例外停止"; // 詳細は最下行メッセージ
    hline += std::string("  ") + "レイアウト " + std::to_string(g_layout + 1) + "/" +
             std::to_string(layout::COUNT) + " " + layout::name(g_layout);
    if (g_layout != layout::COUNT - 1) {
      std::string hidden;
      if (!g_showReg) hidden += " レジスタ";
      if (!g_showBt) hidden += " バックトレース";
      if (!g_showTh) hidden += " スレッド";
      if (!hidden.empty()) hline += "  (非表示:" + hidden + ")";
    }
  }
  if (g_session.symbolsWarming()) { // シンボル先読みの進捗（固まって見えないように）
    char w[64]; snprintf(w, sizeof(w), "  ⏳シンボル先読み %d/%d", g_session.symbolsWarmedDone(), g_session.symbolsWarmedTotal());
    hline += w;
  }
  if (g_jobRunning.load() && g_jobShowing.load()) { // 重いシンボル探索中（UI は固まらない・ESC で中止）
    char w[80]; snprintf(w, sizeof(w), "  🔍シンボル探索 %d/%d（ESC で中止）", g_jobDone.load(), g_jobTotal.load());
    hline += w;
  }
  hline += " \x1b[0m";
  g_term.putAt(0, 0, hline);

  // 参照コマンドの出力オーバレイ（ページャ: ↑↓/Space/b でスクロール、ESC・q で閉じる）
  if (!g_output.empty()) {
    int top = 2, avail = std::max(1, rows - 1 - top);
    int total = (int)g_output.size();
    int maxTop = std::max(0, total - avail);
    if (g_outputTop > maxTop) g_outputTop = maxTop;
    if (g_outputTop < 0) g_outputTop = 0;
    std::string hint = g_xActive ? "Enter 続き / " : "";
    hint += "↑↓ Space/b ページ  g/G 先頭/末尾  ESC・q 閉じる";
    g_term.putAt(0, 1, "\x1b[33m" + g_outputTitle + "\x1b[0m \x1b[2m（" + hint + "）\x1b[0m");
    for (int y = 0; y < avail; y++) {
      int idx = g_outputTop + y;
      std::string t = (idx < total) ? g_output[idx] : "";
      g_term.putAt(0, top + y, layout::fitWidth(t, cols));
    }
    g_term.clearLine(rows - 1);
    std::string foot;
    if (total > avail) { char b[64]; snprintf(b, sizeof(b), "%d–%d / %d 行", g_outputTop + 1, std::min(total, g_outputTop + avail), total); foot = b; }
    if (!g_message.empty()) foot += (foot.empty() ? "" : "    ") + g_message;
    if (!foot.empty()) g_term.putAt(0, rows - 1, "\x1b[2m" + foot + "\x1b[0m");
    g_term.flush();
    return;
  }

  if (g_state == UiState::Stopped) {
    auto L = layout::compute(g_layout, cols, rows, g_showReg, g_showBt, g_showTh);
    drawDisassembly(L.disassemble);
    drawRegisters(L.registers);
    drawBacktrace(L.backtrace);
    drawThreads(L.threads);
  } else if (g_state == UiState::Running) {
    g_term.putAt(0, 2, "(実行中。ブレイクに当たると停止します。Ctrl+C で割り込み停止)");
  } else {
    g_term.putAt(0, 2, ":attach <pid> / :run <パス> で開始    :help でコマンド一覧    :q で終了");
  }

  // 最下行: コマンド入力 or メッセージ
  g_term.clearLine(rows - 1);
  if (g_cmdMode) {
    std::string line = ":" + g_cmdline;
    if (!g_message.empty()) line += "    \x1b[2m" + g_message + "\x1b[0m"; // 補完候補など
    g_term.putAt(0, rows - 1, line);
  }
  else if (!g_message.empty()) g_term.putAt(0, rows - 1, g_message);
  g_term.flush();
}

// ----- プロセス選択のキー処理 -----
void handlePickerKey(int c) {
  if (g_escState == 1) {           // 直前が ESC
    g_escState = 0;
    if (c == '[') { g_escState = 2; return; }  // 矢印などの CSI
    closePicker(); render(); return;            // 単独 ESC → 取消
  }
  if (g_escState == 2) {           // ESC [ の続き
    g_escState = 0;
    if (c == 'A') pickerMove(-1);       // ↑
    else if (c == 'B') pickerMove(+1);  // ↓
    else if (c == 'C') pickerScrollH(+1); // → 選択行の引数を右へスクロール
    else if (c == 'D') pickerScrollH(-1); // ← 戻す
    render();
    return;
  }
  if (c == 27) { g_escState = 1; return; }       // ESC 列の可能性
  if (c == '\r' || c == '\n') {
    auto f = pickerFiltered();
    if (!f.empty() && g_pickSel >= 0 && g_pickSel < (int)f.size()) {
      pid_t pid = f[g_pickSel]->pid;
      closePicker();
      doAttach(pid);
    } else {
      closePicker();
    }
    render();
    return;
  }
  if (c == 16) { pickerMove(-1); render(); return; }  // Ctrl-P 行↑
  if (c == 14) { pickerMove(+1); render(); return; }  // Ctrl-N 行↓
  if (c == 6)  { pickerPage(+1); render(); return; } // Ctrl-f 次ページ
  if (c == 2)  { pickerPage(-1); render(); return; } // Ctrl-b 前ページ
  if (c == 127 || c == 8) {                           // Backspace
    if (!g_pickFilter.empty()) g_pickFilter.pop_back();
    g_pickSel = 0; g_pickTop = 0; g_pickHScroll = 0; render(); return;
  }
  if (c >= 0x20 && c < 0x7f) {                         // 文字入力でフィルタ
    g_pickFilter.push_back((char)c);
    g_pickSel = 0; g_pickTop = 0; g_pickHScroll = 0; render(); return;
  }
}

// 1 個の値を書式 fmt・サイズ size で整形（x:16進 d:符号付 u:符号無 o:8進 t:2進 c:文字 a:アドレス f:浮動小数）
std::string formatValue(uint64_t v, char fmt, int size) {
  if (size < 8) v &= (size == 1 ? 0xFFULL : size == 2 ? 0xFFFFULL : 0xFFFFFFFFULL);
  char b[96];
  switch (fmt) {
    case 'd': { // 符号付（size から符号拡張）
      uint64_t sv = v;
      if (size < 8) { uint64_t sign = 1ULL << (size*8 - 1); if (v & sign) sv = v | ~((1ULL<<(size*8))-1); }
      snprintf(b, sizeof(b), "%lld", (long long)sv); break;
    }
    case 'u': snprintf(b, sizeof(b), "%llu", (unsigned long long)v); break;
    case 'o': snprintf(b, sizeof(b), "0%llo", (unsigned long long)v); break;
    case 't': { std::string s; for (int i = size*8 - 1; i >= 0; i--) { s += ((v>>i)&1) ? '1' : '0'; if (i && i%8==0) s += ' '; } return s; }
    case 'c': { uint8_t c = (uint8_t)(v & 0xFF);
      if (c >= 0x20 && c < 0x7f) snprintf(b, sizeof(b), "0x%02x '%c'", c, (char)c);
      else snprintf(b, sizeof(b), "0x%02x", c); break; }
    case 'a': { std::string w = g_session.describe(v); snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); return std::string(b) + (w.empty() ? "" : ("  " + w)); }
    case 'f': { if (size == 4) { float f; uint32_t u=(uint32_t)v; memcpy(&f,&u,4); snprintf(b,sizeof(b),"%g",(double)f); } else { double d; memcpy(&d,&v,8); snprintf(b,sizeof(b),"%g",d); } break; }
    case 'x': default: snprintf(b, sizeof(b), "0x%0*llx", size*2, (unsigned long long)v); break;
  }
  return b;
}

// gdb 風メモリ検査。spec = "[個数][書式][サイズ]"（例: 4xg / 8c / 10i / s）。結果を g_output へ。
void examineMemory(const std::string& spec, uint64_t addr) {
  int count = 0; char fmt = 'x'; int size = 0; size_t i = 0;
  while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') { count = count*10 + (spec[i]-'0'); i++; }
  for (; i < spec.size(); i++) {
    char c = spec[i];
    if (c=='b') size=1; else if (c=='h') size=2; else if (c=='w') size=4; else if (c=='g') size=8;
    else if (c=='x'||c=='d'||c=='u'||c=='o'||c=='t'||c=='c'||c=='i'||c=='s'||c=='f'||c=='a') fmt=c;
    else { g_message = std::string("不明な書式指定: ") + c + "（書式 x/d/u/o/t/c/i/s/f/a, サイズ b/h/w/g）"; return; }
  }
  if (count <= 0) count = (fmt=='c') ? 32 : (fmt=='s') ? 4 : 8;
  if (size <= 0) size = (fmt=='c') ? 1 : 8;
  if (fmt=='c') size = 1;
  if (fmt=='f' && size < 4) size = 8;

  g_output.clear();
  char th[96];

  if (fmt == 'i') { // 逆アセンブル count 命令
    std::vector<uint8_t> code((size_t)count * 16 + 16, 0); size_t got = 0;
    g_session.target().readMemory(addr, code.data(), code.size(), &got);
    g_session.maskBreakpoints(addr, code.data(), got);
    Disassembler dis(g_session.arch());
    auto insns = dis.disasm(code.data(), got, addr, count);
    snprintf(th, sizeof(th), "逆アセンブル 0x%llx （%d 命令）", (unsigned long long)addr, count); g_outputTitle = th;
    for (const auto& in : insns) {
      const Image* im = g_baseRelative ? g_session.images().byAddress(in.address) : nullptr;
      uint64_t dispAddr = (g_baseRelative && im) ? (in.address - im->loadAddr) : in.address;
      char line[256]; snprintf(line, sizeof(line), "0x%012llx  %-7s %s", (unsigned long long)dispAddr, in.mnemonic.c_str(), in.opStr.c_str());
      std::string s(line);
      if (in.hasCallTarget) { std::string t = g_session.symbolName(in.callTarget); if (!t.empty()) s += "  ; " + t; }
      g_output.push_back(s);
    }
    g_xNextAddr = insns.empty() ? addr : (insns.back().address + insns.back().size); // 継続先
    return;
  }

  if (fmt == 's') { // null 終端文字列を count 本
    snprintf(th, sizeof(th), "文字列 0x%llx", (unsigned long long)addr); g_outputTitle = th;
    for (int k = 0; k < count; k++) {
      std::string str; uint8_t c = 0; uint64_t a = addr; int n = 0;
      for (; n < 4096; n++) { if (!g_session.target().readMemory(a + n, &c, 1)) { c = 0; break; } if (c == 0) break;
        str += (c >= 0x20 && c < 0x7f) ? (char)c : '.'; }
      char head[32]; snprintf(head, sizeof(head), "0x%012llx  ", (unsigned long long)a);
      g_output.push_back(std::string(head) + "\"" + str + "\"");
      addr = a + n + 1;
    }
    g_xNextAddr = addr; // 最後の文字列の次（継続先）
    return;
  }

  // 数値 / 文字
  size_t total = (size_t)count * size;
  std::vector<uint8_t> buf(total, 0); size_t got = 0;
  if (!g_session.target().readMemory(addr, buf.data(), total, &got) || got == 0) {
    char m[64]; snprintf(m, sizeof(m), "0x%llx は読めません", (unsigned long long)addr); g_message = m; return;
  }
  int avail = (int)(got / (size_t)size); if (avail < count) count = avail;
  snprintf(th, sizeof(th), "メモリ 0x%llx  /%d%c%c", (unsigned long long)addr, count, fmt,
           size==1?'b':size==2?'h':size==4?'w':'g'); g_outputTitle = th;
  int perLine = (fmt=='a') ? 1 : size>=8 ? 2 : size>=4 ? 4 : size>=2 ? 8 : 8;
  for (int idx = 0; idx < count; ) {
    char head[32]; snprintf(head, sizeof(head), "0x%012llx  ", (unsigned long long)(addr + (size_t)idx*size));
    std::string s(head);
    for (int j = 0; j < perLine && idx < count; j++, idx++) {
      uint64_t v = 0; for (int bb = 0; bb < size; bb++) v |= (uint64_t)buf[(size_t)idx*size + bb] << (8*bb);
      s += formatValue(v, fmt, size) + "  ";
    }
    g_output.push_back(s);
  }
  g_xNextAddr = addr + (size_t)count * size; // 継続先
}

// 従来の 16 進＋ASCII ダンプ。bytes バイト表示し、継続先アドレスを覚える。
void classicDump(uint64_t addr, int bytes) {
  if (bytes < 1) bytes = 1; if (bytes > 4096) bytes = 4096;
  std::vector<uint8_t> buf(bytes, 0); size_t got = 0;
  if (!g_session.target().readMemory(addr, buf.data(), bytes, &got) || got == 0) {
    char m[64]; snprintf(m, sizeof(m), "0x%llx は読めません", (unsigned long long)addr); g_message = m; return;
  }
  g_output.clear(); char th[64]; snprintf(th, sizeof(th), "メモリ 0x%llx (%zu バイト)", (unsigned long long)addr, got); g_outputTitle = th;
  for (size_t i = 0; i < got; i += 16) {
    char line[160]; int n = snprintf(line, sizeof(line), "0x%012llx  ", (unsigned long long)(addr + i));
    std::string s(line, n); std::string asc;
    for (size_t j = 0; j < 16; j++) {
      if (i + j < got) { char hx[4]; snprintf(hx, sizeof(hx), "%02x ", buf[i+j]); s += hx; uint8_t c = buf[i+j]; asc += (c>=0x20&&c<0x7f) ? (char)c : '.'; }
      else s += "   ";
      if (j == 7) s += " ";
    }
    g_output.push_back(s + " |" + asc + "|");
  }
  g_xNextAddr = addr + got; // 継続先
}

// 直近の examine（書式/バイト数）を g_xNextAddr から続けて表示する。
void xContinue() {
  if (g_xSpec.empty()) classicDump(g_xNextAddr, g_xClassicBytes);
  else examineMemory(g_xSpec, g_xNextAddr);
  g_outputTop = 0; // 新しいチャンクは先頭から
  if (!g_output.empty()) g_xActive = true;
}

// "a,b,c" を最大 maxParts 個に分割（式にカンマは無い前提）。余り（パス等）は最後の要素に残す。
std::vector<std::string> splitComma(const std::string& s, int maxParts) {
  std::vector<std::string> out; size_t i = 0;
  while ((int)out.size() < maxParts - 1) {
    size_t c = s.find(',', i);
    if (c == std::string::npos) break;
    out.push_back(s.substr(i, c - i)); i = c + 1;
  }
  out.push_back(s.substr(i));
  return out;
}
std::string trimStr(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t"); return s.substr(a, b - a + 1);
}
// HEX 文字列（空白無視）→ バイト列。空・奇数長・不正文字で false。
bool parseHexBytes(const std::string& in, std::vector<uint8_t>& out) {
  std::string h; for (char c : in) if (!std::isspace((unsigned char)c)) h += c;
  if (h.empty() || h.size() % 2) return false;
  auto nib = [](char c) -> int { if (c>='0'&&c<='9') return c-'0'; if (c>='a'&&c<='f') return c-'a'+10; if (c>='A'&&c<='F') return c-'A'+10; return -1; };
  for (size_t i = 0; i < h.size(); i += 2) { int hi = nib(h[i]), lo = nib(h[i+1]); if (hi<0||lo<0) return false; out.push_back((uint8_t)(hi*16+lo)); }
  return true;
}

// ----- コマンド実行 -----
// シンボル式 expr を評価し、完了したら apply(ok, addr, err) を呼ぶ。
// TUI では別スレッドで評価して UI を固めない（重い全画像探索は進捗表示＋ESC 中止可）。
// バッチではイベントループが無いのでその場で同期評価して即 apply する。
void evalAsync(const std::string& expr,
               std::function<void(bool, uint64_t, const std::string&)> apply) {
  if (!g_interactive) { // バッチ: 同期
    Expression ex(g_session);
    uint64_t a = 0; std::string e;
    bool ok = ex.eval(expr, a, e);
    apply(ok, a, e);
    return;
  }
  if (g_jobRunning.load()) { g_message = "処理中です（ESC で中止）"; return; }
  g_jobCancel = false; g_jobShowing = false; g_jobDone = 0; g_jobTotal = 0;
  g_jobApply = std::move(apply);
  g_jobRunning = true;
  g_jobThread = std::thread([expr]() {
    Expression ex(g_session);
    ex.setAsync(&g_jobCancel, [](int done, int total) {
      g_jobDone.store(done); g_jobTotal.store(total);
      g_jobShowing.store(true);          // 重い探索に入った → 進捗表示を有効化
      g_queue.push({Event::Progress, 0}); // メインに再描画を促す
    });
    uint64_t a = 0; std::string e;
    bool ok = false;
    try { ok = ex.eval(expr, a, e); }
    catch (const std::exception& ex2) { ok = false; e = std::string("内部エラー: ") + ex2.what(); }
    catch (...) { ok = false; e = "内部エラー（不明）"; }
    g_jobOk = ok; g_jobAddr = a; g_jobErr = e;
    g_queue.push({Event::JobDone, 0});    // メインが結果を適用
  });
}

// 引数を「#番号」または式として解釈し、ブレイクの id を fn(id) に渡す（bd/bt 用）。
// #番号 は式評価が不要なのでその場で即 fn を呼ぶ。式は evalAsync 経由で評価し（重い全画像
// 探索でも UI を固めない）、完了後にアドレス→ブレイク id を引いて fn を呼ぶ。
// 失敗時は g_message を設定し fn は呼ばない。
void withBreakId(const std::string& arg, std::function<void(int)> fn) {
  if (!arg.empty() && arg[0] == '#') { fn(atoi(arg.c_str() + 1)); return; }
  evalAsync(arg, [fn](bool ok, uint64_t addr, const std::string& eerr) {
    if (!ok) { g_message = "式エラー: " + eerr; return; }
    const Breakpoint* b = g_session.breaks().userAt(addr);
    if (!b) { g_message = "そのアドレスにブレイクは無い"; return; }
    fn(b->id);
  });
}

void runCommand(const std::string& line) {
  // 非同期ジョブ（シンボル探索）実行中は次のコマンドを受けない（対象状態を同時に触らない）。
  if (g_jobRunning.load()) { g_message = "処理中です（ESC で中止してください）"; return; }
  g_message.clear();
  g_outputTop = 0; // 新しいコマンド出力は先頭から表示
  // コマンド実行前のこの境界で、イメージ一覧とユーザ注釈を最新化する。
  //  - refreshImages: dlopen で後から読み込まれたライブラリにも :b / :sym が効くように
  //    （dyld の変更タイムスタンプを見て、変化時だけ再読込するので走行中でも安価）。
  //    停止時は onStoppedEvent でも更新するが、非停止で走行中のまま打たれた場合をここで拾う。
  //  - refreshAnnotations: 外部編集された注釈ファイルを読み直す（条件2）。重い探索を別スレッドへ
  //    投げる前のこの境界で行うので、ワーカが読むイメージ／注釈データは一貫している。
  if (g_session.isAttached()) { g_session.refreshImages(); g_session.refreshAnnotations(); }
  std::string cmd = line, arg;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }

  if (cmd == "q" || cmd == "quit") {
    g_session.detach();
    g_term.leave();
    std::exit(0);
  } else if (cmd == "attach") {
    if (g_session.isAttached()) { g_message = "既に attach 済み（先に :detach）"; return; }
    if (arg.empty()) { openPicker(); return; }   // pid 無し → 一覧から選択
    doAttach((pid_t)parseNum(arg));
  } else if (cmd == "run" || cmd == "launch") {
    if (g_session.isAttached()) { g_message = "既に attach/起動 済み（先に :detach）"; return; }
    if (arg.empty()) { g_message = "使い方: run <実行ファイルパス> [引数...]"; return; }
    std::vector<std::string> args; { std::string cur; for (char ch : arg) { if (ch==' ') { if(!cur.empty()){args.push_back(cur);cur.clear();} } else cur.push_back(ch); } if(!cur.empty()) args.push_back(cur); }
    doLaunch(args);
  } else if (cmd == "detach") {
    g_session.detach();
    g_state = UiState::Idle;
    g_message = "detached";
  } else if (cmd == "auto") {
    if (g_state != UiState::Stopped) { g_message = "auto は停止中のみ（まずブレイク等で停止してから）"; return; }
    std::vector<std::string> toks; { std::string cur; for (char ch : arg) { if (ch==' ') { if(!cur.empty()){toks.push_back(cur);cur.clear();} } else cur.push_back(ch); } if(!cur.empty()) toks.push_back(cur); }
    int mask = 0;
    if (toks.empty()) mask = 1 | 2 | 4;             // 既定: call/syscall/ret すべて
    else for (auto& t : toks) {
      if (t == "call") mask |= 1;
      else if (t == "syscall" || t == "sc") mask |= 2;
      else if (t == "ret") mask |= 4;
      else if (t == "all") mask |= 7;
      else { g_message = "指定は call / syscall(sc) / ret / all（省略で all）"; return; }
    }
    g_autoMask = mask; g_autoSteps = 0;
    g_session.stepInto(); g_state = UiState::Running; // 以降は Stopped ハンドラが自動継続
    g_message = "auto 実行中…（Ctrl+C で中断）";
  } else if (cmd == "b") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    evalAsync(arg, [](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      const Breakpoint* bp = g_session.breaks().add(addr);
      if (bp) {
        std::string w = g_session.describe(addr);
        char buf[96]; snprintf(buf, sizeof(buf), "#%d ブレイク設定 0x%llx", bp->id, (unsigned long long)addr);
        g_message = std::string(buf) + (w.empty() ? "" : ("  " + w));
      } else {
        g_message = "ブレイク設定失敗（メモリに書けない？）";
      }
    });
  } else if (cmd == "bh") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    evalAsync(arg, [](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      const Breakpoint* bp = g_session.breaks().addHw(addr);
      if (bp) {
        g_session.syncHwBreaks(); // デバッグレジスタへ反映
        std::string w = g_session.describe(addr);
        char buf[96]; snprintf(buf, sizeof(buf), "#%d HW ブレイク設定 0x%llx", bp->id, (unsigned long long)addr);
        g_message = std::string(buf) + (w.empty() ? "" : ("  " + w));
      } else {
        char buf[80]; snprintf(buf, sizeof(buf), "HW ブレイク空きなし（最大 %d 本）", g_session.breaks().hwCapacity());
        g_message = buf;
      }
    });
  } else if (cmd == "w") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    if (arg.empty()) { g_message = "使い方: w <式> [サイズ 1/2/4/8] [r|w|rw]（既定 8 書込）"; return; }
    // 空白区切り: 先頭=式、残り=サイズ/種別（順不同）
    std::vector<std::string> toks; { std::string cur; for (char ch : arg) { if (ch==' ') { if(!cur.empty()){toks.push_back(cur);cur.clear();} } else cur.push_back(ch); } if(!cur.empty()) toks.push_back(cur); }
    std::string expr = toks[0]; int size = 8; bool onR = false, onW = false;
    for (size_t i = 1; i < toks.size(); i++) {
      const std::string& t = toks[i];
      if (t=="1"||t=="2"||t=="4"||t=="8") size = atoi(t.c_str());
      else if (t=="r"||t=="w"||t=="rw"||t=="wr") { onR = t.find('r')!=std::string::npos; onW = t.find('w')!=std::string::npos; }
      else { g_message = "不明な指定: " + t + "（サイズは 1/2/4/8、種別は r/w/rw）"; return; }
    }
    if (!onR && !onW) onW = true; // 既定は書込監視
    evalAsync(expr, [size, onR, onW](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      if (addr % (uint64_t)size != 0) { char m[96]; snprintf(m, sizeof(m), "アドレス 0x%llx は %d バイト境界に未整列", (unsigned long long)addr, size); g_message = m; return; }
      const Breakpoint* wp = g_session.breaks().addWatch(addr, size, onR, onW);
      if (wp) {
        g_session.syncHwBreaks();
        const char* mode = (onR&&onW) ? "R/W" : onR ? "R" : "W";
        std::string nm = g_session.describe(addr);
        char m[160]; snprintf(m, sizeof(m), "#%d ウォッチ設定 0x%llx %dバイト %s", wp->id, (unsigned long long)addr, size, mode);
        g_message = std::string(m) + (nm.empty() ? "" : ("  " + nm));
      } else {
        char m[80]; snprintf(m, sizeof(m), "ウォッチ空きなし（最大 %d 本）", g_session.breaks().watchCapacity());
        g_message = m;
      }
    });
  } else if (cmd == "bl") {
    const auto& bl = g_session.breaks().list();
    if (bl.empty()) { g_message = "ブレイクポイントなし（:b <式> で設定）"; return; }
    std::string s;
    for (const auto& b : bl) {
      char head[96]; snprintf(head, sizeof(head), "#%d 0x%llx", b.id, (unsigned long long)b.addr);
      std::string one = head;
      std::string w = g_session.symbolName(b.addr);
      if (!w.empty()) one += " " + w;
      if (b.watch) { char t[32]; snprintf(t, sizeof(t), " [W %s%dB]", (b.onRead&&b.onWrite)?"rw":b.onRead?"r":"w", b.watchSize); one += t; }
      else if (b.hw) one += " [HW]";
      if (!b.enabled) one += " [無効]";
      if (b.hits) { char h[24]; snprintf(h, sizeof(h), " (%lluhit)", (unsigned long long)b.hits); one += h; }
      if (!s.empty()) s += "   ";
      s += one;
    }
    g_message = s;
  } else if (cmd == "bd") {
    if (arg.empty()) { g_message = "使い方: bd <#番号 | 式>"; return; }
    withBreakId(arg, [](int id) { // #番号 は即時、式は非同期評価のあと
      if (g_session.breaks().removeId(id)) { g_session.syncHwBreaks(); char m[32]; snprintf(m, sizeof(m), "#%d を削除", id); g_message = m; }
      else g_message = "そのブレイクは無い";
    });
  } else if (cmd == "bt") {
    if (arg.empty()) { g_message = "使い方: bt <#番号 | 式>（指定したブレイクの有効⇄無効を切替）"; return; }
    withBreakId(arg, [](int id) { // #番号 は即時、式は非同期評価のあと
      const Breakpoint* b = g_session.breaks().byId(id);
      if (!b) { g_message = "そのブレイクは無い"; return; }
      bool want = !b->enabled;
      if (g_session.breaks().setEnabled(id, want)) { g_session.syncHwBreaks(); char m[48]; snprintf(m, sizeof(m), "#%d を%s", id, want ? "有効化" : "無効化"); g_message = m; }
      else g_message = "切替に失敗";
    });
  } else if (cmd == "sym") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    evalAsync(arg, [](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      std::string w = g_session.describe(addr);
      char buf[64]; snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
      g_message = std::string(buf) + (w.empty() ? "  (どのイメージにも入っていない)" : ("  " + w));
    });
  } else if (cmd == "dis" || cmd == "disas" || cmd == "disassemble") {
    // 逆アセンブル画面（ペイン）を移動する。dis <式> でそのアドレスから、dis で現在 PC へ。
    // オーバレイ（ページャ）ではなく本体の逆アセンブル表示を動かす。以降 ↑↓/Ctrl+F で移動可。
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    if (g_state != UiState::Stopped) { g_message = "dis は停止中のみ（逆アセンブル画面の移動）"; return; }
    std::string a = trimStr(arg);
    if (a.empty()) {
      g_disasmFollow = true; g_disasmTrail.clear();           // 現在 PC へ戻す（追従）
      g_message = "逆アセンブル: 現在位置(PC)へ";
    } else {
      evalAsync(a, [](bool ok, uint64_t addr, const std::string& eerr) {
        if (!ok) { g_message = "式エラー: " + eerr; return; }
        if (g_state != UiState::Stopped) { g_message = "dis は停止中のみ（逆アセンブル画面の移動）"; return; } // 評価中に状態が変わった場合
        g_disasmTop = addr; g_disasmFollow = false; g_disasmTrail.clear(); g_focus = Focus::Disasm;
        std::string w = g_session.describe(addr);
        g_message = "逆アセンブル: " + (w.empty() ? "0x" : w) + " から（↑↓/Ctrl+F で移動、s/n/c で現在位置へ）";
      });
    }
  } else if (cmd == "reg") {
    if (g_state != UiState::Stopped) { g_message = "レジスタは停止中のみ参照／変更できます"; return; }
    const void* ts = g_session.stopState().data();
    size_t eq = arg.find('=');
    if (eq != std::string::npos) { // 書き込み  reg <名前>=<式>
      auto trim = [](std::string s) { size_t a = s.find_first_not_of(' '); if (a == std::string::npos) return std::string(); size_t b = s.find_last_not_of(' '); return s.substr(a, b - a + 1); };
      std::string name = trim(arg.substr(0, eq)), vexpr = trim(arg.substr(eq + 1));
      if (name.empty() || vexpr.empty()) { g_message = "使い方: reg <名前>=<式>"; return; }
      evalAsync(vexpr, [name](bool ok, uint64_t val, const std::string& eerr) {
        if (!ok) { g_message = "式エラー: " + eerr; return; }
        if (g_state != UiState::Stopped) { g_message = "レジスタは停止中のみ参照／変更できます"; return; } // 評価中に状態が変わった場合
        if (g_session.setRegister(name, val)) { char m[80]; snprintf(m, sizeof(m), "%s = 0x%llx に設定", name.c_str(), (unsigned long long)val); g_message = m; }
        else g_message = "書き込めないレジスタ: " + name;
      });
      return;
    }
    // ベクタ/浮動小数レジスタ（xmmN / vN、または xmm/v で全部）。128bit なので別経路で表示。
    {
      std::string a = arg; if (!a.empty() && (a[0]=='$'||a[0]=='%')) a = a.substr(1);
      bool wantAll = (a=="xmm" || a=="v" || a=="vec" || a=="fp" || a=="fpregs");
      bool wantOne = (a.rfind("xmm",0)==0 && a.size()>3) || (a.size()>=2 && a[0]=='v' && std::isdigit((unsigned char)a[1]));
      if (wantAll || wantOne) {
        std::vector<uint8_t> fs;
        if (!g_session.target().getFloatState(g_session.stopThread(), fs)) { g_message = "ベクタレジスタを取得できない"; return; }
        auto vregs = g_session.arch().vectorRegisters(fs.data());
        if (vregs.empty()) { g_message = "このアーキはベクタレジスタ未対応"; return; }
        g_output.clear(); g_outputTitle = "ベクタレジスタ";
        for (auto& vr : vregs) {
          if (wantOne && vr.name != a) continue;
          std::string hex = "0x"; char hx[3];
          for (int k = 15; k >= 0; k--) { snprintf(hx, sizeof(hx), "%02x", vr.bytes[k]); hex += hx; } // 上位→下位で読める向きに
          char b[96]; snprintf(b, sizeof(b), "%-6s = %s", vr.name.c_str(), hex.c_str());
          g_output.push_back(b);
        }
        if (g_output.empty()) { g_message = "不明なベクタレジスタ: " + a; g_outputTitle.clear(); }
        return;
      }
    }
    if (!arg.empty()) { // 1 レジスタ参照
      uint64_t v = 0;
      if (g_session.arch().readRegisterByName(ts, arg, v)) {
        char m[80]; snprintf(m, sizeof(m), "%s = 0x%llx (%lld)", arg.c_str(), (unsigned long long)v, (long long)v);
        g_message = m;
      } else g_message = "不明なレジスタ: " + arg;
      return;
    }
    auto regs = g_session.arch().registers(ts);
    g_output.clear(); g_outputTitle = "レジスタ";
    for (auto& r : regs) {
      char b[96]; snprintf(b, sizeof(b), "%-6s = 0x%016llx  (%lld)", r.name.c_str(), (unsigned long long)r.value, (long long)r.value);
      g_output.push_back(b);
    }
  } else if (cmd == "x" || cmd.rfind("x/", 0) == 0) {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    // 書式指定は "x/4xg 式" でも "x /4xg 式" でも可。無指定なら従来の 16 進ダンプ。
    std::string spec; if (cmd.size() > 2) spec = cmd.substr(2); // "x/..." 形式
    std::vector<std::string> toks; { std::string cur; for (char ch : arg) { if (ch==' ') { if(!cur.empty()){toks.push_back(cur);cur.clear();} } else cur.push_back(ch); } if(!cur.empty()) toks.push_back(cur); }
    if (spec.empty() && !toks.empty() && !toks[0].empty() && toks[0][0] == '/') { spec = toks[0].substr(1); toks.erase(toks.begin()); }
    // 式が無ければ「前回の続き」。書式だけ変えて続けることもできる（例: x/i → x）。
    if (toks.empty()) {
      if (!g_xHasLast) { g_message = "使い方: x <式> [バイト数]  /  x/<個数><書式><サイズ> <式>（続きは引数なし x か Enter）"; return; }
      if (!spec.empty()) g_xSpec = spec; // 続きで書式変更
      xContinue();
      return;
    }
    evalAsync(toks[0], [spec, toks](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      g_xSpec = spec;
      if (spec.empty()) { // 従来の 16 進ダンプ（バイト数指定可）
        g_xClassicBytes = 64;
        if (toks.size() > 1) { g_xClassicBytes = (int)parseNum(toks[1]); }
        classicDump(addr, g_xClassicBytes);
      } else {
        examineMemory(spec, addr);
      }
      if (!g_output.empty()) { g_xHasLast = true; g_xActive = true; }
    });
  } else if (cmd == "img" || cmd == "libs") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    g_session.refreshImages();
    const auto& imgs = g_session.images().images();
    g_output.clear(); g_outputTitle = "イメージ " + std::to_string(imgs.size()) + " 個（:syms <名前> でシンボル一覧）";
    for (const auto& im : imgs) {
      char b[320]; snprintf(b, sizeof(b), "0x%012llx  +0x%08llx  %s", (unsigned long long)im.loadAddr, (unsigned long long)im.textSize, im.name.c_str());
      g_output.push_back(b);
    }
  } else if (cmd == "syms") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    g_session.refreshImages();
    // 第1語=ライブラリ名（省略で既定ライブラリ）、残り=絞り込み（部分一致・大小無視）
    std::vector<std::string> toks; { std::string cur; for (char ch : arg) { if (ch==' ') { if(!cur.empty()){toks.push_back(cur);cur.clear();} } else cur.push_back(ch); } if(!cur.empty()) toks.push_back(cur); }
    const Image* im = nullptr; std::string filt;
    if (toks.empty()) im = g_session.defaultLibImage();
    else { im = g_session.images().byName(toks[0]); for (size_t i = 1; i < toks.size(); i++) filt += (i>1?" ":"") + toks[i]; }
    if (!im) { g_message = toks.empty() ? "対象ライブラリが不明" : ("ライブラリが見つからない: " + toks[0]); return; }
    std::string lf = filt; std::transform(lf.begin(), lf.end(), lf.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    const auto& syms = g_session.symbols().forImage(*im);
    g_output.clear();
    int shown = 0;
    for (const auto& s : syms) {
      const std::string& dn = g_session.symbols().displayName(s); // 遅延デマングル（この一覧ぶんだけ）
      if (!lf.empty()) {
        std::string hay = dn + " " + s.mangled;
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (hay.find(lf) == std::string::npos) continue;
      }
      char b[384]; snprintf(b, sizeof(b), "0x%012llx  %s", (unsigned long long)s.addr, dn.c_str());
      g_output.push_back(b); shown++;
    }
    g_outputTitle = im->name + " のシンボル " + std::to_string(shown) + " 個"
                  + (filt.empty() ? "" : ("（絞込: " + filt + "）"));
    if (g_output.empty()) g_output.push_back(filt.empty() ? "(シンボルなし)" : "(一致なし)");
  } else if (cmd == "uselib") {
    std::string a = trimStr(arg);
    if (a.empty()) {
      const std::string& d = g_session.defaultLib();
      g_message = d.empty() ? "既定ライブラリ: (自動＝主実行ファイル)  変更: uselib <名前> / 解除: uselib -"
                            : ("既定ライブラリ: " + d + "  （解除: uselib -）");
    } else if (a == "-") {
      g_session.setDefaultLib("");
      g_message = "既定ライブラリを自動（主実行ファイル）に戻した";
    } else {
      if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
      const Image* im = g_session.images().byName(a);
      if (!im) { g_message = "ライブラリが見つからない: " + a; return; }
      g_session.setDefaultLib(a);
      g_message = "既定ライブラリ: " + im->name + "（式のシンボル名はここを優先して探す）";
    }
  } else if (cmd == "ann") {
    // 自作バイナリにユーザ自身が名前（＋任意でメモ）を付ける。UUID 別のテキストに保存し、
    // 以降 :b <名前> / :sym / 式 で引けて、describe/バックトレースにも出る（条件3）。
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    auto parts = splitComma(arg, 3); // <式>,<名前>[,<メモ>]
    if (parts.size() < 2) { g_message = "使い方: ann <式>,<名前>[,<メモ>]（名前を付ける。メモだけなら :memo）"; return; }
    std::string name = trimStr(parts[1]);
    std::string memo = parts.size() >= 3 ? trimStr(parts[2]) : std::string();
    if (name.empty()) { g_message = "名前が空です（メモだけ付けるなら :memo <式>,<メモ>）"; return; }
    evalAsync(trimStr(parts[0]), [name, memo](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      const Image* im = g_session.images().byAddress(addr);
      if (!im) { g_message = "そのアドレスはどのイメージにも入っていません"; return; }
      std::string err;
      bool r = g_session.annotations().setName(*im, addr, name, err);
      if (r && !memo.empty()) r = g_session.annotations().setMemo(*im, addr, memo, err);
      if (!r) { g_message = "注釈を保存できません: " + err; return; }
      char buf[96]; snprintf(buf, sizeof(buf), "注釈: 0x%llx = %s", (unsigned long long)addr, name.c_str());
      g_message = std::string(buf) + (memo.empty() ? "" : ("  ; " + memo)) + "  → " + Annotations::pathFor(*im);
    });
  } else if (cmd == "memo") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    auto parts = splitComma(arg, 2); // <式>,<メモ>
    if (parts.size() < 2) { g_message = "使い方: memo <式>,<メモ>（そのアドレスにメモを付ける／空メモで消す）"; return; }
    std::string memo = trimStr(parts[1]);
    evalAsync(trimStr(parts[0]), [memo](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      const Image* im = g_session.images().byAddress(addr);
      if (!im) { g_message = "そのアドレスはどのイメージにも入っていません"; return; }
      std::string err;
      if (!g_session.annotations().setMemo(*im, addr, memo, err)) { g_message = "メモを保存できません: " + err; return; }
      char buf[96]; snprintf(buf, sizeof(buf), "メモ: 0x%llx", (unsigned long long)addr);
      g_message = std::string(buf) + (memo.empty() ? " を削除" : ("  ; " + memo)) + "  → " + Annotations::pathFor(*im);
    });
  } else if (cmd == "unann") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    if (trimStr(arg).empty()) { g_message = "使い方: unann <式>（そのアドレスの注釈を削除）"; return; }
    evalAsync(trimStr(arg), [](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      const Image* im = g_session.images().byAddress(addr);
      if (!im) { g_message = "そのアドレスはどのイメージにも入っていません"; return; }
      std::string err;
      if (!g_session.annotations().remove(*im, addr, err)) { g_message = err; return; }
      char buf[80]; snprintf(buf, sizeof(buf), "注釈を削除: 0x%llx", (unsigned long long)addr);
      g_message = buf;
    });
  } else if (cmd == "anns") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    g_session.refreshImages();
    const Image* im = trimStr(arg).empty() ? g_session.images().mainExecutable()
                                           : g_session.images().byName(trimStr(arg));
    if (!im) { g_message = trimStr(arg).empty() ? "主実行ファイルが見つからない" : ("イメージが見つからない: " + trimStr(arg)); return; }
    g_session.annotations().refresh(*im);
    auto anns = g_session.annotations().list(*im);
    g_output.clear();
    std::string p = Annotations::pathFor(*im);
    g_outputTitle = im->name + " の注釈 " + std::to_string(anns.size()) + " 個" + (p.empty() ? "（UUID 無し＝保存不可）" : ("  [" + p + "]"));
    for (const auto& a : anns) {
      uint64_t rt = a.unslid + im->slide;
      char b[512];
      snprintf(b, sizeof(b), "0x%012llx  (unslid 0x%llx)  %s%s%s",
               (unsigned long long)rt, (unsigned long long)a.unslid,
               a.name.empty() ? "(名前なし)" : a.name.c_str(),
               a.memo.empty() ? "" : "   ; ", a.memo.c_str());
      g_output.push_back(b);
    }
    if (g_output.empty()) g_output.push_back("(注釈なし)  :ann <式>,<名前>[,<メモ>] で追加、またはファイルを直接編集");
  } else if (cmd == "scwatch") {
    // syscall 観測（libdtrace 相乗り）。止めずに名前別の回数を集計する（strace 的）。
    // 開始は attach が要るが、状況表示・停止は対象終了後でも使える（集計は残る）。
    std::string a = trimStr(arg);
    if (a == "off") {
      if (!g_session.syscallWatchActive()) { g_message = "syscall 監視は動いていません"; return; }
      g_session.stopSyscallWatch();
      g_message = "syscall 監視を停止しました";
    } else if (a.empty()) {
      bool on = g_session.syscallWatchActive();
      uint64_t total = g_session.syscallWatchTotal();
      if (!on && total == 0) { g_message = "syscall 監視: 停止中（:scwatch <名前|all> で開始）"; return; }
      auto counts = g_session.syscallWatchCounts();
      std::sort(counts.begin(), counts.end(),
                [](const std::pair<std::string, uint64_t>& x, const std::pair<std::string, uint64_t>& y) { return x.second > y.second; });
      g_output.clear();
      g_outputTitle = std::string("syscall 監視 ") + (on ? "[稼働中 " : "[停止 ") + g_session.syscallWatchFilter() +
                      "]  総計 " + std::to_string(total) + " 回" + (on ? "（:scwatch off で停止）" : "");
      for (const auto& c : counts) { char b[288]; snprintf(b, sizeof(b), "%-28s %llu", c.first.c_str(), (unsigned long long)c.second); g_output.push_back(b); }
      if (g_output.empty()) g_output.push_back("(まだ発火なし)");
    } else {
      if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
      std::string err;
      std::string filter = (a == "all") ? std::string() : a; // "" = 全件
      if (!g_session.startSyscallWatch(filter, err)) { g_message = "syscall 監視の開始に失敗: " + err; return; }
      g_message = "syscall 監視を開始 [" + (a == "all" ? std::string("all") : a) + "]（:scwatch で状況 / :scwatch off で停止）";
    }
  } else if (cmd == "scstop") {
    // syscall 停止キャッチポイント（Phase 2）。マッチした syscall/Mach トラップが発火した瞬間に
    // カーネル（dtrace の stop()）が対象を凍結し、通常のブレイクと同じく検査できる。cont で次の
    // 発火まで走る。dtrace ハンドルは観測版（:scwatch）と共用なので両者は排他。filter 書式も同じ。
    std::string a = trimStr(arg);
    if (a == "off") {
      if (!(g_session.syscallWatchActive() && g_session.syscallStopMode())) { g_message = "停止キャッチポイントは設定されていません"; return; }
      g_session.stopSyscallWatch();
      g_message = "syscall 停止キャッチポイントを解除しました";
    } else if (a.empty()) {
      bool on = g_session.syscallWatchActive() && g_session.syscallStopMode();
      if (!on) { g_message = "syscall 停止キャッチポイント: 未設定（:scstop <名前|all|mach[:<名前>]> で設定）"; return; }
      g_message = std::string("syscall 停止キャッチポイント: 稼働中 [") + g_session.syscallWatchFilter() +
                  "]  命中 " + std::to_string(g_session.syscallWatchTotal()) + " 回（:scstop off で解除）";
    } else {
      if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
      std::string err;
      std::string filter = (a == "all") ? std::string() : a; // "" = 全件
      if (!g_session.startSyscallStop(filter, err)) { g_message = "停止キャッチポイントの設定に失敗: " + err; return; }
      g_message = "syscall 停止キャッチポイントを設定 [" + a + "]（発火で停止。:scstop off で解除）";
    }
  } else if (cmd == "seg") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    const Image* im = arg.empty() ? g_session.images().mainExecutable() : g_session.images().byName(arg);
    if (!im) { g_message = arg.empty() ? "主実行ファイルが見つからない" : ("イメージが見つからない: " + arg); return; }
    g_output.clear(); g_outputTitle = "セグメント " + im->name + "（slide 0x" + [&]{ char h[24]; snprintf(h,sizeof(h),"%llx",(unsigned long long)im->slide); return std::string(h); }() + "）";
    for (const auto& s : im->segments) {
      uint64_t a = s.vmaddr + im->slide;
      char b[320]; snprintf(b, sizeof(b), "0x%012llx-0x%012llx  %-12s  vmsize 0x%llx", (unsigned long long)a, (unsigned long long)(a + s.vmsize), s.name.c_str(), (unsigned long long)s.vmsize);
      g_output.push_back(b);
    }
    if (g_output.empty()) g_output.push_back("(セグメント情報なし)");
  } else if (cmd == "p" || cmd.rfind("p/", 0) == 0) {
    // 式の値を表示（p=10進+16進, p/x=16進, p/d=10進）。
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    char fmt = (cmd.size() > 2) ? cmd[2] : 0;
    if (arg.empty()) { g_message = "使い方: p <式>（p/x 16進, p/d 10進）"; return; }
    evalAsync(arg, [fmt, arg](bool ok, uint64_t v, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      char m[128];
      if (fmt == 'x') snprintf(m, sizeof(m), "0x%llx", (unsigned long long)v);
      else if (fmt == 'd') snprintf(m, sizeof(m), "%lld", (long long)v);
      else snprintf(m, sizeof(m), "0x%llx (%lld)", (unsigned long long)v, (long long)v);
      g_message = trimStr(arg) + " = " + m;
    });
  } else if (cmd == "setmem" || cmd == "setmem1" || cmd == "setmem2" || cmd == "setmem4" || cmd == "setmem8") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    auto parts = splitComma(arg, 2);
    if (parts.size() < 2) { g_message = "使い方: setmem[1/2/4/8] <式>,<値>  /  setmem <式>,<HEX列>"; return; }
    // 値側は先にその場で解釈し、アドレス式だけ非同期評価する（重いシンボル探索でも UI を固めない）。
    std::vector<uint8_t> bytes;
    if (cmd == "setmem") { // バイト列（HEX）をそのまま書く
      if (!parseHexBytes(parts[1], bytes)) { g_message = "HEX 列が不正（偶数桁の 16 進で。例: setmem $rsp,9090CC）"; return; }
    } else {              // N バイトの値を書く（リトルエンディアン）
      int size = cmd[6] - '0';
      Expression ex(g_session); uint64_t val = 0; std::string eerr;
      if (!ex.eval(trimStr(parts[1]), val, eerr)) { g_message = "式エラー: " + eerr; return; }
      for (int i = 0; i < size; i++) bytes.push_back((uint8_t)((val >> (8*i)) & 0xff));
    }
    evalAsync(trimStr(parts[0]), [bytes](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      if (g_session.target().writeMemory(addr, bytes.data(), bytes.size())) {
        char m[96]; snprintf(m, sizeof(m), "0x%llx に %zu バイト書き込み", (unsigned long long)addr, bytes.size()); g_message = m;
      } else g_message = "書き込み失敗（アドレスを確認）";
    });
  } else if (cmd == "memset") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    auto parts = splitComma(arg, 3);
    if (parts.size() < 3) { g_message = "使い方: memset <式>,<値>,<サイズ>"; return; }
    // 値・サイズは先にその場で解釈し、アドレス式だけ非同期評価する。
    Expression ex(g_session); uint64_t val = 0, size = 0; std::string eerr;
    if (!ex.eval(trimStr(parts[1]), val, eerr) || !ex.eval(trimStr(parts[2]), size, eerr)) {
      g_message = "式エラー: " + eerr; return;
    }
    if (size == 0 || size > (1u << 24)) { g_message = "サイズは 1〜16MB"; return; }
    evalAsync(trimStr(parts[0]), [val, size](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      std::vector<uint8_t> buf((size_t)size, (uint8_t)(val & 0xff));
      if (g_session.target().writeMemory(addr, buf.data(), buf.size())) {
        char m[96]; snprintf(m, sizeof(m), "0x%llx を 0x%02x で %llu バイト埋めた", (unsigned long long)addr, (unsigned)(val&0xff), (unsigned long long)size); g_message = m;
      } else g_message = "書き込み失敗";
    });
  } else if (cmd == "wf" || cmd == "rf") {
    if (!g_session.isAttached()) { g_message = "先に attach してください"; return; }
    auto parts = splitComma(arg, 3); // addr, size, path（path は残り全部）
    if (parts.size() < 3) { g_message = std::string("使い方: ") + cmd + " <式>,<サイズ>,<パス>"; return; }
    // サイズ・パスは先にその場で解釈し、アドレス式だけ非同期評価する。
    Expression ex(g_session); uint64_t size = 0; std::string eerr;
    if (!ex.eval(trimStr(parts[1]), size, eerr)) { g_message = "式エラー: " + eerr; return; }
    std::string path = trimStr(parts[2]);
    if (path.empty() || size == 0 || size > (1u << 28)) { g_message = "パス必須・サイズは 1〜256MB"; return; }
    evalAsync(trimStr(parts[0]), [cmd, size, path](bool ok, uint64_t addr, const std::string& eerr) {
      if (!ok) { g_message = "式エラー: " + eerr; return; }
      if (cmd == "wf") { // 対象メモリ → ファイル
        std::vector<uint8_t> buf((size_t)size); size_t got = 0;
        if (!g_session.target().readMemory(addr, buf.data(), buf.size(), &got) || got == 0) { g_message = "メモリを読めない"; return; }
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { g_message = "ファイルを開けない: " + path; return; }
        size_t wrote = fwrite(buf.data(), 1, got, f); fclose(f);
        char m[160]; snprintf(m, sizeof(m), "0x%llx から %zu バイトを書き出し → %s", (unsigned long long)addr, wrote, path.c_str()); g_message = m;
      } else {           // ファイル → 対象メモリ
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { g_message = "ファイルを開けない: " + path; return; }
        std::vector<uint8_t> buf((size_t)size); size_t got = fread(buf.data(), 1, buf.size(), f); fclose(f);
        if (got == 0) { g_message = "ファイルが空か読めない"; return; }
        if (g_session.target().writeMemory(addr, buf.data(), got)) {
          char m[160]; snprintf(m, sizeof(m), "%s の %zu バイトを 0x%llx へ書き込み", path.c_str(), got, (unsigned long long)addr); g_message = m;
        } else g_message = "メモリへ書き込めない";
      }
    });
  } else if (cmd == "intel" || cmd == "atandt" || cmd == "att") {
    bool intel = (cmd == "intel");
    Disassembler::setIntelSyntax(intel); // 以降に生成される逆アセンブラへ効く（x86 のみ）
    g_message = std::string("逆アセンブル記法: ") + (intel ? "Intel" : "AT&T") + "（次の描画から）";
  } else if (cmd == "baseoff" || cmd == "baseon") {
    g_baseRelative = (cmd == "baseoff"); // baseoff=イメージ先頭からの相対値 / baseon=実アドレス(既定)
    g_message = std::string("逆アセンブルのアドレス表示: ") + (g_baseRelative ? "イメージ先頭からの相対値" : "実アドレス");
  } else if (cmd == "dr") {
    if (g_state != UiState::Stopped) { g_message = "dr は停止中のみ参照できます"; return; }
    std::vector<uint8_t> ds(g_session.arch().debugStateCount() * sizeof(natural_t), 0);
    if (!g_session.target().getDebugState(g_session.stopThread(), ds.data())) { g_message = "デバッグレジスタを取得できない"; return; }
    auto drs = g_session.arch().debugRegisters(ds.data());
    if (drs.empty()) { g_message = "このアーキはデバッグレジスタ表示未対応"; return; }
    g_output.clear(); g_outputTitle = "デバッグレジスタ";
    for (auto& r : drs) { char b[80]; snprintf(b, sizeof(b), "%-9s = 0x%016llx", r.name.c_str(), (unsigned long long)r.value); g_output.push_back(b); }
  } else if (cmd == "help" || cmd == "h" || cmd == "?") {
    g_output.clear(); g_outputTitle = "ta2debug ヘルプ";
    const char* H[] = {
      "\x1b[36m― キー操作 ―\x1b[0m",
      "  c 継続    s ステップイン    n ステップオーバ    Ctrl+C 実行中を割り込み停止",
      "  :auto [call|syscall|ret|all]  興味のある命令の手前まで自動継続（省略で all、Ctrl+C で中断）",
      "  : コマンド入力    l / L レイアウト切替    1 / 2 / 3 レジスタ/バックトレース/スレッド表示切替    0 全表示",
      "  ↑ / ↓ 逆アセンブルを 1 命令スクロール    Ctrl+F / Ctrl+B 1 ページ送り（s/n/c で現在位置へ戻る）",
      "  Tab 逆アセンブル ⇔ スレッド選択 を切替（スレッド選択中は ↑↓ で表示スレッドを切替。★=停止 👁=表示中）",
      "  q 終了",
      "  出力オーバレイ（help/img/libs/syms/reg/dis/x 等）: ↑↓/Space/b ページ  g/G 先頭末尾  ESC・q 閉じる  Enter 続き(x/dis)",
      "\x1b[36m― アタッチ／起動 ―\x1b[0m",
      "  :attach [pid]              pid 省略でプロセス一覧から選択",
      "  :run <パス> [引数...]      バイナリを起動してデバッグ（エントリ手前で停止）",
      "  :detach                    デタッチ（attach は動き続ける／:run した子は終了させる）",
      "\x1b[36m― ブレイクポイント ―\x1b[0m",
      "  :b  <式>                   ソフトブレイク（INT3/BRK）",
      "  :bh <式>                   ハードウェア実行ブレイク（デバッグレジスタ）",
      "  :w  <式> [1/2/4/8] [r|w|rw] ウォッチポイント（データ監視・既定 8 バイト書込）",
      "  :bl                        一覧（● SW / ◆ HW / [W] ウォッチ, [無効] / (Nhit)）",
      "  :bd <#番号|式>             削除       :bt <#番号|式>  ブレイクの有効⇄無効を切替",
      "\x1b[36m― 参照 ―\x1b[0m",
      "  :reg [名前]                レジスタ一覧／1 本（停止中）",
      "  :reg xmm[N] / vN           ベクタ/浮動小数レジスタ（x86 xmm0-15 / arm64 v0-31、128bit）",
      "  :dr                        デバッグレジスタ（x86 dr0-7 / arm64 bvr/bcr/wvr/wcr）",
      "  :p <式>  :p/x  :p/d         式の値を表示（10進+16進 / 16進 / 10進）",
      "  :intel / :atandt           逆アセンブルの記法を Intel / AT&T(既定) に切替（x86）",
      "  :baseoff / :baseon         逆アセンブルのアドレスを イメージ先頭相対 / 実アドレス(既定) で表示",
      "  :dis [<式>]                逆アセンブル画面をそのアドレスへ移動（省略で現在 PC へ。以降 ↑↓/Ctrl+F）",
      "  :x <式> [バイト数]         メモリ 16 進ダンプ",
      "  :x/<個数><書式><サイズ> <式>  書式付き（書式 x d u o t c i s f a / サイズ b h w g）",
      "  :x                         前回の続き（オーバレイ表示中は Enter でも）",
      "  :sym <式>                  アドレスのシンボル    :img / :libs  読込イメージ（ライブラリ）一覧    :seg [名前]  セグメント一覧",
      "  :syms <ライブラリ> [絞込]   そのライブラリのシンボル一覧（省略で既定ライブラリ・絞込は部分一致）",
      "  :uselib [<名前>|-]          式でライブラリ名省略時に先に探す既定ライブラリを 表示 / 設定 / 解除",
      "\x1b[36m― 注釈（自分で付ける名前・メモ） ―\x1b[0m",
      "  :ann <式>,<名前>[,<メモ>]   そのアドレスに名前を付ける（以降 :b <名前> / 式で引ける・表示にも出る）",
      "  :memo <式>,<メモ>           そのアドレスにメモを付ける（空メモで削除）    :unann <式>  注釈を削除",
      "  :anns [<ライブラリ>]        注釈一覧（省略で主実行ファイル）。保存先ファイルのパスも表示",
      "\x1b[36m― システムコール監視（libdtrace・止めずに観測） ―\x1b[0m",
      "  :scwatch <名前|all>        BSD syscall を監視（例 :scwatch write / :scwatch all）",
      "  :scwatch mach[:<名前>]     Mach トラップを監視（例 :scwatch mach / :scwatch mach:mach_msg_trap）。表示は mach: 付き",
      "  :scwatch                   監視状況（名前ごとの回数）を表示    :scwatch off  監視停止",
      "  :scstop <名前|all|mach[:<名前>]>  その syscall/トラップ発火で対象を停止（ブレイク相当。c で次の発火まで走る）",
      "  :scstop                    停止キャッチポイントの状況    :scstop off  解除（:scwatch と排他）",
      "  注釈は UUID 別のテキストに保存（~/Library/Application Support/ta2debug/annotations/<UUID>.t2a）。",
      "  手編集でき（TAB 区切り）、起動中に外部で更新されても自動で読み直します。stripped な自作バイナリに有効。",
      "\x1b[36m― 書き換え ―\x1b[0m",
      "  :reg <名前>=<式>           レジスタへ書込（al/ah/ax/eax/r8w 等の部分レジスタも可）",
      "  :setmem1/2/4/8 <式>,<値>   N バイトの値を書込      :setmem <式>,<HEX列>  バイト列（例 $rsp,9090CC）",
      "  :memset <式>,<値>,<サイズ> 1 バイト値で埋める",
      "  :wf <式>,<サイズ>,<パス>   メモリをファイルへ書出  :rf <式>,<サイズ>,<パス>  ファイルをメモリへ",
      "\x1b[36m― 例外（クラッシュ）検知 ―\x1b[0m",
      "  不正メモリ参照・不正命令・算術例外で対象が落ちると自動で停止し ⚠ を表示します。",
      "  停止後はレジスタ/メモリ/バックトレースを確認でき、c で対象を通常どおり終了させます。",
      "\x1b[36m― 自動解析（操作不要・停止時に出る） ―\x1b[0m",
      "  バックトレース: CFI（compact unwind / DWARF eh_frame）で正確化。関数入口・IP=0・stub でも呼び元を復元。",
      "  syscall 直前で停まると呼ぶ内容を解析表示（例 write(1, 0x.., 14) \"hi\"）。Mach トラップ・引数フラグ・mach_msg も分解。",
      "  PLT/スタブは \"symbol stub for: X\" と解決表示。C++ 名はデマングルしテンプレートを省略（std::map<…>）。",
      "\x1b[36m― その他 ―\x1b[0m",
      "  :help / :h / :?            このヘルプ    :q  終了",
      "  非対話: ta2debug -b <対象> -o \"cmd\" …（AI/スクリプト向け。-h で詳細）",
      "",
      "\x1b[2m式には 16進(0x..)/10進、シンボル名、レジスタ名、main+0x10 のような加減算が使えます。\x1b[0m",
    };
    for (const char* line : H) g_output.push_back(line);
  } else {
    g_message = "不明なコマンド: " + cmd + "（:help でコマンド一覧）";
  }
}

// コマンド名の TAB 補完（最初の語のみ。引数部では何もしない）。
void completeCommand() {
  if (g_cmdline.find(' ') != std::string::npos) return;
  static const char* CMDS[] = {"attach", "detach", "run", "launch", "auto", "sym", "quit", "p", "setmem", "memset", "wf", "rf", "intel", "atandt", "baseoff", "baseon", "dr", "dis", "libs", "syms", "uselib", "b", "bh", "bl", "bd", "bt",
                               "w", "reg", "x", "img", "seg", "help", "ann", "memo", "unann", "anns", "scwatch", "scstop"};
  std::vector<std::string> cands;
  for (const char* c : CMDS)
    if (std::string(c).rfind(g_cmdline, 0) == 0) cands.push_back(c); // 接頭辞一致
  if (cands.empty()) { g_message = "補完候補なし"; return; }
  if (cands.size() == 1) {
    g_cmdline = cands[0];
    if (cands[0] == "attach" || cands[0] == "sym") g_cmdline += " "; // 引数を取るものは空白を足す
    g_message.clear();
    return;
  }
  // 複数候補: 共通接頭辞まで伸ばし、候補を一覧表示
  std::string common = cands[0];
  for (size_t i = 1; i < cands.size(); i++) {
    size_t k = 0;
    while (k < common.size() && k < cands[i].size() && common[k] == cands[i][k]) k++;
    common.resize(k);
  }
  if (common.size() > g_cmdline.size()) g_cmdline = common;
  std::string list;
  for (auto& c : cands) { if (!list.empty()) list += " "; list += c; }
  g_message = "候補: " + list;
}

// 新しい停止のたびに表示状態を初期化（PC 追従・停止スレッド表示に戻す）。TUI 専用。
void resetViewOnStop() {
  g_disasmFollow = true; g_disasmTop = 0; g_disasmTrail.clear(); g_viewThread = 0; g_focus = Focus::Disasm; g_mainEsc = 0;
  auto ths = g_session.target().threads();
  g_threadSel = 0;
  for (int i = 0; i < (int)ths.size(); i++) if (ths[i] == g_session.stopThread()) { g_threadSel = i; break; }
}

// 逆アセンブル先頭を instrs 命令動かす（正=下/前方、負=上/後方）。
// 前方: 実デコードの境界を履歴へ積む（常に正確）。後方: 履歴を取り出して正確に戻し、
// 履歴を越える分だけ disasmBack で辿る。これで「前方 n → 後方 n」が必ず元に戻る。
void moveDisasm(int instrs) {
  uint64_t top = g_disasmFollow ? viewPc() : g_disasmTop;
  if (instrs > 0) {
    size_t need = (size_t)instrs * 15 + 16;
    std::vector<uint8_t> code(need, 0); size_t got = 0;
    g_session.target().readMemory(top, code.data(), need, &got);
    g_session.maskBreakpoints(top, code.data(), got);
    Disassembler dis(g_session.arch()); auto in = dis.disasm(code.data(), got, top, instrs + 1);
    int steps = std::min(instrs, (int)in.size() - 1);
    for (int i = 0; i < steps; i++) g_disasmTrail.push_back(in[i].address); // 通過した境界を記録
    if (steps > 0) top = in[steps].address;
  } else if (instrs < 0) {
    int k = -instrs;
    while (k > 0 && !g_disasmTrail.empty()) { top = g_disasmTrail.back(); g_disasmTrail.pop_back(); k--; }
    if (k > 0) top = disasmBack(top, k); // 履歴より上へ（新規領域）は前方整列で推定
  }
  g_disasmTop = top; g_disasmFollow = false;
}
void scrollDisasm(int dir) { moveDisasm(dir > 0 ? +1 : -1); }                               // ↑↓: 1 命令
void pageDisasm(int dir) { moveDisasm(dir > 0 ? +std::max(1, g_disasmHeight - 1) : -std::max(1, g_disasmHeight - 1)); } // Ctrl+F/B: 1 ページ
// スレッド選択を動かし、表示中スレッドを切り替える（dir=+1 下 / -1 上）。
// 注: task_threads() の並びは呼び出しごとに変わりうるので、索引ではなく「現在の表示スレッドを
//     一覧内で探して隣へ」で辿る（tid 基準）。表示順の安定のため tid でソートする。
void selectThread(int dir) {
  auto ths = g_session.target().threads();
  if (ths.empty()) return;
  std::sort(ths.begin(), ths.end());
  thread_act_t cur = g_viewThread ? g_viewThread : g_session.stopThread();
  int idx = 0;
  for (int i = 0; i < (int)ths.size(); i++) if (ths[i] == cur) { idx = i; break; }
  idx += dir;
  if (idx < 0) idx = 0;
  if (idx >= (int)ths.size()) idx = (int)ths.size() - 1;
  g_viewThread = ths[idx]; g_threadSel = idx; // 表示を切替（レジスタ/逆アセンブル/bt が追従）
  g_disasmFollow = true; g_disasmTrail.clear(); // 切替直後は新スレッドの PC へ追従
}

// ----- キー処理 -----
void onKey(int c) {
  // シンボル探索ジョブが進捗表示中なら ESC で中止（最優先。矢印キーの ESC 列より先に拾う）。
  if (g_jobRunning.load() && g_jobShowing.load() && c == 27) {
    g_jobCancel.store(true); g_message = "中止しています…"; render(); return;
  }
  if (g_pickerMode) { handlePickerKey(c); return; }
  if (!g_output.empty()) { // オーバレイ（ページャ）: スクロール／ページ送り、ESC・q で閉じる
    int avail = std::max(1, g_term.rows() - 3);
    int maxTop = std::max(0, (int)g_output.size() - avail);
    auto closeOv = []() { g_output.clear(); g_outputTitle.clear(); g_xActive = false; g_outputTop = 0; g_ovEsc = 0; };
    if (g_ovEsc == 1) { if (c == '[') { g_ovEsc = 2; return; } g_ovEsc = 0; closeOv(); render(); return; } // 単独 ESC → 閉じる
    if (g_ovEsc == 2) { g_ovEsc = 0;                                                                        // ESC[ の続き（矢印）
      if (c == 'A') g_outputTop = std::max(0, g_outputTop - 1);
      else if (c == 'B') g_outputTop = std::min(maxTop, g_outputTop + 1);
      render(); return;
    }
    if (c == 27) { g_ovEsc = 1; return; }                                            // ESC 列の可能性
    if (c == 'q') { closeOv(); render(); return; }
    if (c == ':') { closeOv(); g_cmdMode = true; g_cmdline.clear(); g_message.clear(); render(); return; } // 閉じてコマンド入力へ
    if (c == '\r' || c == '\n') { if (g_xActive) xContinue(); else g_outputTop = std::min(maxTop, g_outputTop + avail); render(); return; }
    if (c == 'j') { g_outputTop = std::min(maxTop, g_outputTop + 1); render(); return; } // 1 行下
    if (c == 'k') { g_outputTop = std::max(0, g_outputTop - 1); render(); return; }      // 1 行上
    if (c == ' ' || c == 6) { g_outputTop = std::min(maxTop, g_outputTop + avail); render(); return; } // Space / Ctrl-F: 1 ページ下
    if (c == 'b' || c == 2) { g_outputTop = std::max(0, g_outputTop - avail); render(); return; }       // b / Ctrl-B: 1 ページ上
    if (c == 'g') { g_outputTop = 0; render(); return; }                               // 先頭
    if (c == 'G') { g_outputTop = maxTop; render(); return; }                           // 末尾
    return;                                                                            // その他は無視（閉じない）
  }
  if (g_cmdMode) {
    if (c == '\r' || c == '\n') { g_cmdMode = false; runCommand(g_cmdline); g_cmdline.clear(); }
    else if (c == 27) { g_cmdMode = false; g_cmdline.clear(); g_message.clear(); } // ESC
    else if (c == '\t') { completeCommand(); }                         // TAB 補完
    else if (c == 127 || c == 8) { if (!g_cmdline.empty()) g_cmdline.pop_back(); g_message.clear(); } // BS
    else if (c >= 0x20 && c < 0x7f) { g_cmdline.push_back((char)c); g_message.clear(); }
    render();
    return;
  }

  // ブレイク中のナビゲーション: 矢印(ESC[A/B)・Tab(フォーカス切替)・Ctrl+F/Ctrl+B(ページ)。
  if (g_state == UiState::Stopped) {
    if (g_mainEsc == 1) { g_mainEsc = (c == '[') ? 2 : 0; return; }       // ESC の続き
    if (g_mainEsc == 2) { g_mainEsc = 0;                                  // ESC [ の続き
      if (c == 'A') { (g_focus == Focus::Threads) ? selectThread(-1) : scrollDisasm(-1); render(); }
      else if (c == 'B') { (g_focus == Focus::Threads) ? selectThread(+1) : scrollDisasm(+1); render(); }
      return;
    }
    if (c == 27) { g_mainEsc = 1; return; }                              // ESC 列の可能性
    if (c == '\t') { g_focus = (g_focus == Focus::Disasm) ? Focus::Threads : Focus::Disasm; render(); return; }
    if (c == 6 && g_focus == Focus::Disasm) { pageDisasm(+1); render(); return; } // Ctrl+F
    if (c == 2 && g_focus == Focus::Disasm) { pageDisasm(-1); render(); return; } // Ctrl+B
  }

  switch (c) {
    case ':': g_cmdMode = true; g_cmdline.clear(); g_message.clear(); render(); break;
    case 's': if (g_state == UiState::Stopped) { g_session.stepInto(); g_state = UiState::Running; render(); } break;
    case 'n': if (g_state == UiState::Stopped) { g_session.stepOver(); g_state = UiState::Running; render(); } break;
    case 'c': if (g_state == UiState::Stopped) { g_session.cont(); g_state = UiState::Running; g_message.clear(); render(); } break;
    case 3:   // Ctrl+C
      if (g_autoMask) { g_autoMask = 0; g_message = "auto 中断"; render(); } // 実行中の step が次に止まる
      else if (g_state == UiState::Running) { g_session.interrupt(); }
      break;
    case 'q': g_session.detach(); g_term.leave(); std::exit(0); break;
    case 'l': g_layout = (g_layout + 1) % layout::COUNT; render(); break;
    case 'L': g_layout = (g_layout + layout::COUNT - 1) % layout::COUNT; render(); break;
    case '1': if (g_state == UiState::Stopped) { g_showReg = !g_showReg; render(); } break;
    case '2': if (g_state == UiState::Stopped) { g_showBt = !g_showBt; render(); } break;
    case '3': if (g_state == UiState::Stopped) { g_showTh = !g_showTh; render(); } break;
    case '0': if (g_state == UiState::Stopped) { g_showReg = g_showBt = g_showTh = true; render(); } break;
    default: break;
  }
}

// 停止イベントの共通処理（TUI メインループ・バッチの両方から呼ぶ）。
// auto 継続中なら自動で踏み続け、停止が確定したら状態とメッセージ（クラッシュ理由 / syscall 注記）を整える。
// 返り値: true=停止確定（呼び出し側が描画／出力する）, false=auto で自動継続した（まだ Running、次の停止を待つ）。
bool onStoppedEvent() {
  g_session.refreshImages();
  g_session.refreshAnnotations(); // ユーザ注釈を（外部編集を含め）読み直し、停止表示・BT に反映（条件2）
  g_session.warmSymbols(); // 初回停止でイメージが揃ったら裏でシンボルを先読み（idempotent）
  if (g_autoMask) {
    uint64_t pc = g_session.stopPc();
    bool crash = g_session.stopException() != 0;
    const Breakpoint* ub = g_session.breaks().userAt(pc);
    bool bpHit = ub && ub->enabled;
    InsnKind k = kindAtPc(pc);
    bool hit = (k == InsnKind::Call && (g_autoMask & 1)) ||
               (k == InsnKind::Syscall && (g_autoMask & 2)) ||
               (k == InsnKind::Return && (g_autoMask & 4));
    if (!crash && !bpHit && !hit && ++g_autoSteps < kAutoMax) {
      g_session.stepInto(); g_state = UiState::Running; // まだ興味なし → さらに 1 命令
      return false;
    }
    int steps = (int)g_autoSteps; g_autoMask = 0; g_autoSteps = 0;
    if (!crash && !bpHit) {
      const char* kind = k == InsnKind::Call ? "call" : k == InsnKind::Syscall ? "syscall" : k == InsnKind::Return ? "ret" : "上限";
      char b[48]; snprintf(b, sizeof(b), "auto → %s で停止（%d 命令）", kind, steps);
      g_message = b;
    }
  }
  g_state = UiState::Stopped;
  if (g_session.stopException() != 0) // クラッシュ停止
    g_message = "⚠ " + g_session.stopReason() + "  — c で対象を通常終了（シグナルを本来のハンドラへ通す）";
  else {
    std::string reason = g_session.stopReason(); // syscall 停止キャッチポイントならその理由（他は空）
    std::string sc = g_session.syscallAtStop();   // syscall 直前なら呼び出し内容
    if (!reason.empty() && !sc.empty()) g_message = reason + "  → " + sc;
    else if (!reason.empty())           g_message = reason;
    else if (!sc.empty())               g_message = "→ syscall: " + sc;
  }
  return true;
}

// ---- 非対話（バッチ）モード: AI やスクリプトからコマンド列で駆動する（lldb -b -o 相当） ----
// TUI・raw モードには入らず、1 コマンドずつ実行してプレーンテキストを stdout へ出す。

// ANSI エスケープ（色・反転）を除去。ヘルプ等 TUI 向け文字列をバッチで素のまま出すため。
std::string stripAnsi(const std::string& s) {
  std::string o; o.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b') { i++; if (i < s.size() && s[i] == '[') { i++; while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) i++; if (i < s.size()) i++; } }
    else o += s[i++];
  }
  return o;
}
void batchPrint(const std::string& s) { std::string t = stripAnsi(s); fwrite(t.data(), 1, t.size(), stdout); fputc('\n', stdout); fflush(stdout); }

// 参照コマンド（reg/x/img/bl…）の結果（g_output / g_message）を出す。
void batchPrintResults() {
  if (!g_outputTitle.empty()) batchPrint(g_outputTitle);
  for (auto& l : g_output) batchPrint(l);
  if (g_output.empty() && !g_message.empty()) batchPrint(g_message);
}

// 停止位置の要約 + バックトレースを出す。
void batchStopReport() {
  uint64_t pc = g_session.stopPc();
  std::string where = g_session.describe(pc);
  std::string head;
  if (where.empty()) { char b[32]; snprintf(b, sizeof(b), "0x%llx", (unsigned long long)pc); head = "stopped @ " + std::string(b); }
  else head = "stopped @ " + where;
  const Breakpoint* ub = g_session.breaks().userAt(pc);
  if (ub && ub->enabled) { char b[48]; snprintf(b, sizeof(b), "  [bp #%d, %lluhit]", ub->id, (unsigned long long)ub->hits); head += b; }
  batchPrint(head);
  SourceLoc loc;
  if (g_session.sourceLineFor(pc, loc)) { // dSYM の DWARF 行情報があれば
    std::string t = loc.text; size_t a0 = t.find_first_not_of(" \t"); if (a0 != std::string::npos) t = t.substr(a0); else t.clear();
    batchPrint("src: " + loc.file + ":" + std::to_string(loc.line) + (t.empty() ? "" : ("  " + t)));
  }
  if (!g_message.empty()) batchPrint(g_message); // クラッシュ理由 / syscall 注記 / auto 停止理由
  batchPrint("backtrace:");
  for (auto& l : computeBacktrace(false)) batchPrint("  " + l);
}

// 停止するはずのコマンド後、Stopped/TargetExited が来るまで待つ。exited=true で対象終了。
void batchWaitForStop(bool& exited) {
  exited = false;
  for (;;) {
    Event e = g_queue.pop();
    if (e.kind == Event::Stopped) { if (onStoppedEvent()) return; /* auto 継続中ならさらに待つ */ }
    else if (e.kind == Event::TargetExited) { g_session.detach(); g_state = UiState::Idle; exited = true; return; }
    // Key はバッチでは発生しない
  }
}

// バッチ用: dis [<式>] [個数] を逆アセンブルして g_output に入れる（baseoff 反映、x で続き）。
bool disToOutput(const std::string& arg) {
  if (!g_session.isAttached()) { g_message = "先に attach してください"; return false; }
  std::vector<std::string> toks; { std::string cur; for (char ch : arg) { if (ch==' ') { if(!cur.empty()){toks.push_back(cur);cur.clear();} } else cur.push_back(ch); } if(!cur.empty()) toks.push_back(cur); }
  uint64_t addr = 0; int count = 20;
  if (toks.empty()) {
    if (g_state != UiState::Stopped) { g_message = "使い方: dis <式> [個数]"; return false; }
    addr = g_session.stopPc();
  } else {
    Expression ex(g_session); std::string eerr;
    if (!ex.eval(toks[0], addr, eerr)) { g_message = "式エラー: " + eerr; return false; }
    if (toks.size() > 1) { int n = (int)parseNum(toks[1]); if (n > 0) count = n; }
  }
  std::string spec = std::to_string(count) + "i";
  examineMemory(spec, addr);
  g_xSpec = spec;
  if (!g_output.empty()) { g_xHasLast = true; g_xActive = true; }
  return true;
}

// 1 行を実行。false を返したら終了（quit）。
bool batchExec(const std::string& raw) {
  size_t a = raw.find_first_not_of(" \t"); if (a == std::string::npos) return true; // 空行
  size_t b = raw.find_last_not_of(" \t");
  std::string line = raw.substr(a, b - a + 1);
  if (line[0] == '#') return true;                 // コメント
  if (line[0] == ':') line = line.substr(1);        // TUI と同じ ':' 綴りも許容
  if (line.empty()) return true;

  std::string cmd = line, arg;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }

  g_message.clear(); g_output.clear(); g_outputTitle.clear();
  fputs("ta2> ", stdout); batchPrint(line);

  if (cmd == "q" || cmd == "quit" || cmd == "exit") return false;

  bool willStop = false; // 停止を待つべきコマンドか
  if (cmd == "c" || cmd == "continue") {
    if (g_state == UiState::Stopped) { g_session.cont(); g_state = UiState::Running; willStop = true; }
    else batchPrint("（停止していません）");
  } else if (cmd == "s" || cmd == "si" || cmd == "step" || cmd == "stepi") {
    if (g_state == UiState::Stopped) { g_session.stepInto(); g_state = UiState::Running; willStop = true; }
    else batchPrint("（停止していません）");
  } else if (cmd == "n" || cmd == "next") {
    if (g_state == UiState::Stopped) { g_session.stepOver(); g_state = UiState::Running; willStop = true; }
    else batchPrint("（停止していません）");
  } else if (cmd == "where" || cmd == "backtrace" || cmd == "frames") {
    if (g_state == UiState::Stopped) { batchPrint("backtrace:"); for (auto& l : computeBacktrace(false)) batchPrint("  " + l); }
    else batchPrint("（停止していません）");
  } else if (cmd == "dis" || cmd == "disas" || cmd == "disassemble") {
    // バッチにはペインが無いので、逆アセンブルをテキスト出力する（x/Ni 相当）。
    if (disToOutput(arg)) batchPrintResults(); else if (!g_message.empty()) batchPrint(g_message);
  } else if (cmd == "wait") {
    if (g_state == UiState::Running) willStop = true;
    else batchPrint("（実行中ではありません）");
  } else {
    runCommand(line);                 // attach/run/auto/detach/b/bh/w/bl/bd/bt/sym/reg/x/img/seg/help は共通
    batchPrintResults();
    bool execCmd = (cmd == "run" || cmd == "launch" || cmd == "auto");
    if (execCmd && g_state == UiState::Running) willStop = true;          // エントリ停止 / auto 停止を待つ
    else if ((cmd == "run" || cmd == "launch") && g_state == UiState::Stopped) { g_message.clear(); batchStopReport(); } // 同期的に停止済み
  }

  if (willStop) {
    bool exited = false;
    g_message.clear(); // コマンド結果の残りを停止レポートに混ぜない（理由は onStoppedEvent が設定する）
    batchWaitForStop(exited);
    if (exited) batchPrint("対象プロセスが終了しました");
    else batchStopReport();
  }
  return true;
}

int runBatch(const std::vector<std::string>& ocmds, const std::string& scriptPath,
             bool haveScript, const std::vector<std::string>& target) {
  g_interactive = false;              // バッチはイベントループが無い → シンボル式は同期評価する
  g_session.setWarmingEnabled(false); // バッチは一度きり。全ライブラリ先読みのオーバーヘッドを省く
  std::vector<std::string> script;
  // 位置引数の対象（全数字=pid / それ以外=パス[+引数]）を先頭コマンドへ
  if (!target.empty()) {
    bool isPid = !target[0].empty();
    for (char ch : target[0]) if (ch < '0' || ch > '9') isPid = false;
    if (isPid) script.push_back("attach " + target[0]);
    else { std::string r = "run"; for (auto& t : target) r += " " + t; script.push_back(r); }
  }
  for (auto& c : ocmds) script.push_back(c);
  if (haveScript) {
    FILE* f = (scriptPath == "-") ? stdin : fopen(scriptPath.c_str(), "r");
    if (!f) { fprintf(stderr, "スクリプトを開けない: %s\n", scriptPath.c_str()); return 1; }
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) { std::string l(buf); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); script.push_back(l); }
    if (f != stdin) fclose(f);
  }
  bool fromStdin = ocmds.empty() && !haveScript; // -o も -s も無ければ標準入力をコマンド源に（パイプ駆動）

  for (auto& l : script) if (!batchExec(l)) { g_session.detach(); return 0; }
  if (fromStdin) {
    char buf[1024];
    while (fgets(buf, sizeof(buf), stdin)) { std::string l(buf); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); if (!batchExec(l)) break; }
  }
  g_session.detach();
  return 0;
}

} // namespace

// バッチ使用法を表示（-h / 不正引数）。
void printUsage() {
  fputs(
    "ta2debug — macOS TUI/バッチ デバッガ（自作プログラム専用）\n"
    "使い方:\n"
    "  ta2debug [pid | パス [引数...]]           対話 TUI で起動\n"
    "  ta2debug -b [対象] -o \"cmd\" [-o ...]      非対話: -o のコマンドを順に実行\n"
    "  ta2debug -b [対象] -s スクリプト           非対話: ファイルの各行を実行（- で標準入力）\n"
    "  ta2debug -b [対象]                         非対話: 標準入力からコマンドを読む\n"
    "  （-o / -s があれば -b は省略可）\n"
    "バッチのコマンド（':' 接頭辞も可）:\n"
    "  run <パス>/attach <pid>/detach  c(継続) s(ステップイン) n(オーバ) wait(停止まで待つ)\n"
    "  where(バックトレース)  b/bh/w(ブレイク)  bl/bd/bt  reg [名]  x[/fmt] <式>  sym/img/seg  auto  help  q\n"
    "  停止すると位置・理由・syscall・バックトレースを自動表示。attach は走行継続なので wait で停止を待つ。\n",
    stdout);
}

int main(int argc, char** argv) {
  // 引数解析: -b バッチ, -o <cmd>（複数可）, -s <file|->, -h ヘルプ, 残りは対象(pid / パス[+引数])
  // （-h は root 不要にしたいので geteuid チェックより前に処理する）
  bool batch = false, haveScript = false;
  std::string scriptPath;
  std::vector<std::string> ocmds, target;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-b" || a == "--batch") batch = true;
    else if (a == "-o") { if (i + 1 < argc) ocmds.push_back(argv[++i]); }
    else if (a == "-s" || a == "--source") { if (i + 1 < argc) { scriptPath = argv[++i]; haveScript = true; } }
    else if (a == "-h" || a == "--help") { printUsage(); return 0; }
    else target.push_back(a);
  }
  if (!ocmds.empty() || haveScript) batch = true; // -o/-s があれば非対話とみなす

  if (geteuid() != 0) { fprintf(stderr, "root 権限で実行してください\n"); return 1; }
  if (batch) return runBatch(ocmds, scriptPath, haveScript, target);

  // ----- 対話 TUI -----
  g_term.enter();
  render();

  // 入力スレッド: 1 バイトずつ読んでキューへ
  std::thread input([] {
    for (;;) {
      unsigned char ch;
      ssize_t n = read(STDIN_FILENO, &ch, 1);
      if (n <= 0) break;
      g_queue.push({Event::Key, (int)ch});
    }
  });
  input.detach();

  // 位置引数: 全数字なら pid として attach、そうでなければパスとして起動（以降は対象への引数）
  if (!target.empty()) {
    bool isPid = !target[0].empty();
    for (char ch : target[0]) if (ch < '0' || ch > '9') isPid = false;
    if (isPid) runCommand("attach " + target[0]);
    else doLaunch(target);
  }
  render();

  for (;;) {
    Event e = g_queue.pop();
    if (e.kind == Event::Key) onKey(e.key);
    else if (e.kind == Event::Progress) { render(); } // シンボル先読み・探索の進捗更新 → 再描画
    else if (e.kind == Event::JobDone) {              // 非同期シンボル探索の完了 → 結果を適用
      if (g_jobThread.joinable()) g_jobThread.join();
      g_jobRunning = false;
      g_jobShowing = false;
      if (!g_jobOk && g_jobCancel.load()) {
        g_message = "中止しました"; // ユーザが ESC で打ち切った
      } else {
        auto apply = std::move(g_jobApply);
        if (apply) apply(g_jobOk, g_jobAddr, g_jobErr);
      }
      g_jobApply = nullptr;
      render();
    }
    else if (e.kind == Event::Stopped) {
      if (onStoppedEvent()) { resetViewOnStop(); render(); } // auto 継続中（false）は描画せず次の停止を待つ
    }
    else if (e.kind == Event::TargetExited) {
      g_session.detach(); // 片付け（死んだ task への操作は無害に失敗する）
      g_state = UiState::Idle;
      g_message = "対象プロセスが終了しました（:attach で再アタッチできます）";
      render();
    }
  }
  return 0;
}
