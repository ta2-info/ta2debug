#include "engine/BreakpointService.h"

#include <cstdio>
#include <utility>

namespace ta2 {
namespace engine {

std::string BreakpointService::uuidHexOf(const Image& img) {
  if (!img.hasUuid) return "";
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02X", img.uuid[i]);
  return std::string(hex, 32);
}

const Breakpoint* BreakpointService::addKind(BpKind kind, uint64_t addr, bool enabled, int wsize,
                                             bool onR, bool onW) {
  const Breakpoint* nb;
  if (kind == BpKind::Watch)       nb = s_.breaks().addWatch(addr, wsize ? wsize : 8, onR, onW);
  else if (kind == BpKind::HwExec) nb = s_.breaks().addHw(addr);
  else                             nb = s_.breaks().add(addr);
  if (nb) { if (!enabled) s_.breaks().setEnabled(nb->id, enabled); s_.syncHwBreaks(); }
  return nb;
}

bool BreakpointService::rebuild(const Breakpoint& oldRef, BpKind newKind, uint64_t newAddr, int wsize,
                                bool onR, bool onW, int& newId) {
  Breakpoint old = oldRef; // removeId で参照先が消えるのでコピーしておく
  bool en = old.enabled;
  BpKind oldKind = kindOf(old);
  s_.breaks().removeId(old.id);
  if (const Breakpoint* nb = addKind(newKind, newAddr, en, wsize, onR, onW)) { newId = nb->id; return true; }
  // ロールバック: 元の種別・アドレスで戻す
  const Breakpoint* rb = addKind(oldKind, old.addr, en, old.watchSize, old.onRead, old.onWrite);
  newId = rb ? rb->id : -1;
  if (!rb) s_.syncHwBreaks(); // 戻せなかった場合も、消した HW ぶんをレジスタへ反映しておく
  return false;
}

bool BreakpointService::remove(int id) {
  if (!s_.breaks().removeId(id)) return false;
  s_.syncHwBreaks();
  return true;
}

bool BreakpointService::toggleEnabled(int id, bool& nowEnabled, bool& found) {
  const Breakpoint* b = s_.breaks().byId(id);
  found = b != nullptr;
  if (!b) return false;
  bool want = !b->enabled;
  if (!s_.breaks().setEnabled(id, want)) return false;
  s_.syncHwBreaks();
  nowEnabled = want;
  return true;
}

bool BreakpointService::applyOne(const BpRecord& r, bool& usedHw) {
  const Image* im = nullptr;
  for (const auto& x : s_.images().images()) if (uuidHexOf(x) == r.imageUuid) { im = &x; break; }
  if (!im) return false; // そのイメージがまだ無い（dlopen 前）
  uint64_t addr = r.unslid + im->slide;
  const Breakpoint* nb;
  if (r.kind == 2)      { nb = s_.breaks().addWatch(addr, r.watchSize ? r.watchSize : 8, r.onRead, r.onWrite); usedHw = true; }
  else if (r.kind == 1) { nb = s_.breaks().addHw(addr); usedHw = true; }
  else                  { nb = s_.breaks().add(addr); }
  if (nb && !r.enabled) s_.breaks().setEnabled(nb->id, false);
  return nb != nullptr;
}

int BreakpointService::applyPending() {
  if (pending_.empty()) return 0;
  int applied = 0; bool anyHw = false;
  std::vector<BpRecord> still;
  for (const auto& r : pending_) {
    if (applyOne(r, anyHw)) applied++;
    else still.push_back(r); // まだ未ロード → 次の停止で再挑戦
  }
  pending_.swap(still);
  if (anyHw) s_.syncHwBreaks();
  return applied;
}

int BreakpointService::restoreSaved() {
  pending_.clear();
  s_.refreshImages();
  const Image* mainImg = s_.images().mainExecutable();
  if (!mainImg) return -1;
  std::string mainUuid = uuidHexOf(*mainImg);
  if (mainUuid.empty()) return -1;
  std::vector<BpRecord> recs;
  if (!BreakpointStore::load(mainUuid, recs)) return -1; // ファイルが無い＝何もしない
  int applied = 0; bool anyHw = false;
  for (const auto& r : recs) {
    if (applyOne(r, anyHw)) applied++;
    else pending_.push_back(r); // 未ロードのイメージ（dlopen 待ち）は保留
  }
  if (anyHw) s_.syncHwBreaks();
  return applied;
}

bool BreakpointService::save(Msg& msg) {
  if (!s_.isAttached()) { msg = Msg::of(MsgId::ErrBpSaveNeedsAttach); return false; }
  s_.refreshImages();
  const Image* mainImg = s_.images().mainExecutable();
  if (!mainImg) { msg = Msg::of(MsgId::ErrBpSaveNoMain); return false; }
  std::string mainUuid = uuidHexOf(*mainImg);
  if (mainUuid.empty()) { msg = Msg::of(MsgId::ErrBpSaveNoUuid); return false; }
  std::vector<BpRecord> recs; int skipped = 0;
  for (const auto& b : s_.breaks().list()) {
    const Image* im = s_.images().byAddress(b.addr);
    std::string u = im ? uuidHexOf(*im) : "";
    if (u.empty()) { skipped++; continue; } // イメージ外（JIT/ヒープ等）は復元不能なので保存しない
    BpRecord r;
    r.imageUuid = u; r.unslid = b.addr - im->slide;
    r.kind = b.watch ? 2 : (b.hw ? 1 : 0);
    r.enabled = b.enabled; r.watchSize = b.watchSize; r.onRead = b.onRead; r.onWrite = b.onWrite;
    recs.push_back(std::move(r));
  }
  Msg err;
  if (!BreakpointStore::save(mainUuid, recs, err)) {
    msg = Msg::of(MsgId::ErrBpSaveFailed).sub("error", err);
    return false;
  }
  std::string path = BreakpointStore::pathForUuid(mainUuid);
  if (skipped) msg = Msg::of(MsgId::BpSavedSkipped).num("n", (int64_t)recs.size()).num("skipped", skipped).str("path", path);
  else         msg = Msg::of(MsgId::BpSaved).num("n", (int64_t)recs.size()).str("path", path);
  return true;
}

int BreakpointService::reapply(const std::vector<Breakpoint>& saved) {
  int reapplied = 0; bool anyHw = false;
  for (const auto& b : saved) {
    const Breakpoint* nb;
    if (b.hw && b.watch) { nb = s_.breaks().addWatch(b.addr, b.watchSize, b.onRead, b.onWrite); anyHw = true; }
    else if (b.hw)       { nb = s_.breaks().addHw(b.addr); anyHw = true; }
    else                 { nb = s_.breaks().add(b.addr); }
    if (nb) { if (!b.enabled) s_.breaks().setEnabled(nb->id, false); reapplied++; }
  }
  if (anyHw) s_.syncHwBreaks();
  return reapplied;
}

} // namespace engine
} // namespace ta2
