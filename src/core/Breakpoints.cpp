#include "core/Breakpoints.h"

namespace ta2 {

// ---- 物理層 ----

bool Breakpoints::addSoft(uint64_t addr) {
  if (orig_.count(addr)) return true; // すでに張ってある
  std::vector<uint8_t> bytes = target_.arch().softBreakBytes();
  std::vector<uint8_t> save(bytes.size());
  if (!target_.readMemory(addr, save.data(), save.size())) return false;
  if (!target_.writeMemory(addr, bytes.data(), bytes.size())) return false;
  orig_[addr] = save;
  return true;
}

bool Breakpoints::removeSoft(uint64_t addr) {
  auto it = orig_.find(addr);
  if (it == orig_.end()) return false;
  bool ok = target_.writeMemory(addr, it->second.data(), it->second.size());
  orig_.erase(it);
  return ok;
}

void Breakpoints::maskInto(uint64_t base, uint8_t* buf, size_t len) const {
  for (const auto& kv : orig_) {
    for (size_t i = 0; i < kv.second.size(); i++) {
      uint64_t a = kv.first + i;
      if (a >= base && a < base + len) buf[a - base] = kv.second[i];
    }
  }
}

void Breakpoints::removeAll() {
  for (auto& kv : orig_) {
    target_.writeMemory(kv.first, kv.second.data(), kv.second.size());
  }
  orig_.clear();
  bps_.clear();
  hwTransientOff_.clear();
}

// HW ブレイク/ウォッチを debugState バッファへ反映（Session がスレッドごとに呼ぶ）
void Breakpoints::applyHwInto(void* ds) const {
  Arch& a = target_.arch();
  int ecap = a.hwBreakCount();
  int wcap = a.hwWatchCount();
  for (int i = 0; i < ecap; i++) a.clearHwExecBreak(ds, i); // いったん全消去
  for (int i = 0; i < wcap; i++) a.clearHwWatch(ds, i);     // （x86 は同じ DR なので二度消しても無害）
  for (const auto& b : bps_) {
    if (!b.hw || !b.enabled || b.slot < 0) continue;
    if (hwTransientOff_.count(b.id)) continue; // ステップ中の一時無効
    if (b.watch) { if (b.slot < wcap) a.setHwWatch(ds, b.slot, b.addr, b.watchSize, b.onRead, b.onWrite); }
    else         { if (b.slot < ecap) a.setHwExecBreak(ds, b.slot, b.addr); }
  }
}

// forWatch 用に空きデバッグレジスタ番号を返す（x86 は実行/ウォッチで DR 共用のため全体を見る）
int Breakpoints::allocHwSlot(bool forWatch) const {
  Arch& a = target_.arch();
  int cap = forWatch ? a.hwWatchCount() : a.hwBreakCount();
  bool shared = a.hwExecWatchSharePool();
  std::vector<bool> used(cap, false);
  for (const auto& b : bps_) {
    if (!b.hw || b.slot < 0) continue;
    bool sameBank = shared || (b.watch == forWatch);
    if (sameBank && b.slot < cap) used[b.slot] = true;
  }
  for (int i = 0; i < cap; i++) if (!used[i]) return i;
  return -1;
}

// ---- ユーザ層 ----

const Breakpoint* Breakpoints::add(uint64_t addr) {
  for (auto& b : bps_) if (b.addr == addr) return &b; // 既存
  if (!addSoft(addr)) return nullptr;                 // 物理設置に失敗
  Breakpoint b;
  b.id = nextId_++;
  b.addr = addr;
  b.enabled = true;
  b.hits = 0;
  bps_.push_back(b);
  return &bps_.back();
}

const Breakpoint* Breakpoints::addHw(uint64_t addr) {
  for (auto& b : bps_) if (!b.watch && b.addr == addr) return &b; // 同アドレスの実行ブレイクは 1 つ
  int slot = allocHwSlot(false);
  if (slot < 0) return nullptr; // 空きデバッグレジスタなし
  Breakpoint b;
  b.id = nextId_++;
  b.addr = addr;
  b.enabled = true;
  b.hits = 0;
  b.hw = true;
  b.slot = slot;
  bps_.push_back(b);
  return &bps_.back();
}

const Breakpoint* Breakpoints::addWatch(uint64_t addr, int size, bool onRead, bool onWrite) {
  if (size != 1 && size != 2 && size != 4 && size != 8) return nullptr;
  if (!onRead && !onWrite) onWrite = true; // 既定は書込監視
  for (auto& b : bps_) if (b.watch && b.addr == addr) return &b; // 同アドレスのウォッチは 1 つ
  int slot = allocHwSlot(true);
  if (slot < 0) return nullptr; // 空きウォッチレジスタなし
  Breakpoint b;
  b.id = nextId_++;
  b.addr = addr;
  b.enabled = true;
  b.hits = 0;
  b.hw = true;
  b.watch = true;
  b.slot = slot;
  b.watchSize = size;
  b.onRead = onRead;
  b.onWrite = onWrite;
  bps_.push_back(b);
  return &bps_.back();
}

bool Breakpoints::removeId(int id) {
  for (auto it = bps_.begin(); it != bps_.end(); ++it) {
    if (it->id == id) {
      if (it->hw) hwTransientOff_.erase(it->id); // レジスタ反映は Session が再同期
      else if (it->enabled) removeSoft(it->addr);
      bps_.erase(it);
      return true;
    }
  }
  return false;
}

bool Breakpoints::removeAddr(uint64_t addr) {
  for (const auto& b : bps_) if (b.addr == addr) return removeId(b.id);
  return false;
}

bool Breakpoints::setEnabled(int id, bool on) {
  for (auto& b : bps_) {
    if (b.id != id) continue;
    if (b.hw) {
      b.enabled = on; // スロットは確保したまま。レジスタ反映は Session が再同期
      return true;
    }
    if (on && !b.enabled) {
      if (!addSoft(b.addr)) return false;
      b.enabled = true;
    } else if (!on && b.enabled) {
      removeSoft(b.addr);
      b.enabled = false;
    }
    return true;
  }
  return false;
}

const Breakpoint* Breakpoints::byId(int id) const {
  for (const auto& b : bps_) if (b.id == id) return &b;
  return nullptr;
}

const Breakpoint* Breakpoints::userAt(uint64_t addr) const {
  for (const auto& b : bps_) if (b.addr == addr) return &b;
  return nullptr;
}

void Breakpoints::onHit(uint64_t addr) {
  for (auto& b : bps_) if (b.addr == addr) { b.hits++; return; }
}

} // namespace ta2
