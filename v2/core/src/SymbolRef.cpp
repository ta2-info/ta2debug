#include "core/SymbolRef.h"

#include <cstdio>

namespace ta2 {

SymbolRef resolveAddress(ImageList& images, Symbols& symbols, Annotations* annots,
                         uint64_t addr, bool wantMemo) {
  SymbolRef r;
  r.addr = addr;
  const Image* img = images.byAddress(addr);
  if (!img) return r;
  r.hasImage = true;
  r.image = img->name;
  r.imageOffset = addr - img->loadAddr;
  if (img->containsStub(addr)) { // __stubs: 直前シンボルに巻き込まれないよう、指す先を明示
    r.inStub = true;
    r.stubTarget = symbols.stubTarget(*img, addr);
    return r;                    // v1 と同じくスタブではシンボル・メモを引かない
  }
  const Symbol* sym = symbols.byAddress(*img, addr);
  if (sym) {
    r.hasSymbol = true;
    r.symbol = symbols.displayName(*sym);
    r.symbolOffset = addr - sym->addr;
  }
  // そのアドレスにユーザのメモがあれば添える（条件3 で付けた自由記述）。
  if (wantMemo && annots)
    if (const Annot* a = annots->atExact(*img, addr))
      if (!a->memo.empty()) r.memo = a->memo;
  return r;
}

std::string formatDescribe(const SymbolRef& r) {
  if (!r.hasImage) return std::string();
  char head[64];
  snprintf(head, sizeof(head), "%s 0x%llx", r.image.c_str(), (unsigned long long)r.imageOffset);
  std::string s(head);
  if (r.inStub)
    return s + (r.stubTarget.empty() ? "  [symbol stub]" : ("  symbol stub for: " + r.stubTarget));
  if (r.hasSymbol) {
    char off[32];
    snprintf(off, sizeof(off), "+0x%llx", (unsigned long long)r.symbolOffset);
    s += "  " + r.symbol + off;
  }
  if (!r.memo.empty()) s += "  ; " + r.memo;
  return s;
}

std::string formatSymbolName(const SymbolRef& r) {
  if (!r.hasImage) return std::string();
  if (r.inStub) return r.stubTarget.empty() ? (r.image + " [stub]") : (r.stubTarget + " [stub]");
  if (!r.hasSymbol) return r.image;
  if (r.symbolOffset == 0) return r.symbol;
  char buf[32];
  snprintf(buf, sizeof(buf), "+0x%llx", (unsigned long long)r.symbolOffset);
  return r.symbol + buf;
}

} // namespace ta2
