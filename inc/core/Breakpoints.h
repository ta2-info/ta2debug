#pragma once

// ソフトブレイク（命令の先頭バイトを Arch::softBreakBytes() に置換）の管理。
// 2 層構造:
//  - 物理層 orig_ : いまメモリに実際に置いてある INT3/BRK と元バイト。Stepper が
//    再張り（rearm）や stepOver の一時ブレイクで透過的に出し入れする。
//  - ユーザ層 bps_ : ユーザが張ったブレイク（id/有効無効/命中回数）。Stepper の一時操作に
//    影響されない「意図」の一覧。有効なユーザブレイクは物理層にも置かれる。
// ハードウェアブレイクは将来 Arch の DR/DBGBVR 経由（本クラスは当面ソフトのみ）。
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "core/Target.h"

namespace ta2 {

struct Breakpoint {
  int id = 0;
  uint64_t addr = 0;
  bool enabled = true;
  uint64_t hits = 0;
  bool hw = false;    // デバッグレジスタを使うか（HW 実行ブレイク or ウォッチ）。false ならソフトブレイク(INT3/BRK)
  bool watch = false; // hw かつ watch=true ならデータウォッチポイント（それ以外の hw は実行ブレイク）
  int slot = -1;      // hw の場合に割り当てたデバッグレジスタ番号（バンク内 0..）
  int watchSize = 0;  // watch の監視バイト数（1/2/4/8）
  bool onRead = false, onWrite = false; // watch の監視アクセス種別
};

class Breakpoints {
public:
  explicit Breakpoints(Target& t) : target_(t) {}

  // ---- ユーザ操作 ----
  // 追加（同アドレスが既にあれば既存を返す）。有効なら物理的に INT3 を置く。失敗で nullptr。
  const Breakpoint* add(uint64_t addr);
  // ハードウェアブレイク追加。空きデバッグレジスタが無ければ nullptr。
  // メモリは書き換えず、実際のレジスタ反映は Session::syncHwBreaks() が担う。
  const Breakpoint* addHw(uint64_t addr);
  // ハードウェアウォッチポイント追加（データアクセス監視）。size は 1/2/4/8。空き無しで nullptr。
  const Breakpoint* addWatch(uint64_t addr, int size, bool onRead, bool onWrite);
  int hwCapacity() const { return target_.arch().hwBreakCount(); }  // 実行ブレイク用レジスタ本数
  int watchCapacity() const { return target_.arch().hwWatchCount(); } // ウォッチ用レジスタ本数
  bool removeId(int id);        // id 指定で削除（物理的にも外す）
  bool removeAddr(uint64_t addr);
  bool setEnabled(int id, bool on);
  const Breakpoint* byId(int id) const;
  const Breakpoint* userAt(uint64_t addr) const; // そのアドレスのユーザブレイク（無ければ nullptr）
  const std::vector<Breakpoint>& list() const { return bps_; }
  void onHit(uint64_t addr);    // 命中時に hits++（該当ユーザブレイクがあれば）

  // ---- 物理層（Stepper が使う）----
  bool addSoft(uint64_t addr);
  bool removeSoft(uint64_t addr);
  bool has(uint64_t addr) const { return orig_.count(addr) != 0; }

  // 逆アセンブル/ダンプ表示用: buf[base..base+len) の INT3/BRK を元バイトへ戻す（表示だけ本来の命令に見せる）
  void maskInto(uint64_t base, uint8_t* buf, size_t len) const;

  // ---- ハードウェアブレイク（Session がスレッドのデバッグ状態へ反映する）----
  // debugState バッファ（arch().debugStateCount()*sizeof(natural_t) バイト）に、有効な
  // HW ブレイクを全スロットぶん書き込む（未使用スロットは消す）。他のフィールド（SS 等）は触らない。
  void applyHwInto(void* debugState) const;
  // ステップで HW ブレイク/ウォッチを一瞬またぐ間だけ、その id を反映対象から外す/戻す。
  void hwTransientDisable(int id) { hwTransientOff_.insert(id); }
  void hwTransientEnable(int id) { hwTransientOff_.erase(id); }

  // デタッチ時: 物理ブレイクを全部外し、ユーザ一覧もクリア
  void removeAll();

private:
  // forWatch=true ならウォッチ用、false なら実行ブレイク用に空きデバッグレジスタ番号を返す（-1=空き無し）。
  // x86 は両者が同じ DR を共用するため全 hw ブレイクを見て重複を避ける。
  int allocHwSlot(bool forWatch) const;

  Target& target_;
  std::map<uint64_t, std::vector<uint8_t>> orig_; // addr -> 元バイト列（物理層）
  std::vector<Breakpoint> bps_;                   // ユーザ層
  std::set<int> hwTransientOff_;                  // ステップ中だけ反映を外す HW ブレイク/ウォッチの id
  int nextId_ = 1;
};

} // namespace ta2
