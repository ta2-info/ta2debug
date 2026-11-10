#include "core/Unwind.h"

#include <cstring>

#include <mach-o/compact_unwind_encoding.h>

namespace ta2 {

// __unwind_info のバイト列をキャッシュ読込（1 回だけ対象から読む）。
const std::vector<uint8_t>* Unwinder::blobFor(const Image& im) {
  uint64_t addr = im.unwindInfoAddr();
  if (!addr || !im.unwindSize) return nullptr;
  auto it = cache_.find(im.loadAddr);
  if (it != cache_.end()) return it->second.empty() ? nullptr : &it->second;

  std::vector<uint8_t> buf(im.unwindSize);
  if (!read_(addr, buf.data(), buf.size())) buf.clear(); // 失敗は空でキャッシュ（再読込しない）
  auto& stored = cache_[im.loadAddr] = std::move(buf);
  return stored.empty() ? nullptr : &stored;
}

// blob の off から len バイトを safe に読む（範囲外なら false）。
static bool pick(const std::vector<uint8_t>& b, size_t off, void* dst, size_t len) {
  if (off + len > b.size()) return false;
  memcpy(dst, b.data() + off, len);
  return true;
}

bool Unwinder::encodingFor(uint64_t pc, uint32_t& encoding, uint64_t& funcStart) {
  const Image* im = images_.byAddress(pc);
  if (!im) return false;
  const std::vector<uint8_t>* blobp = blobFor(*im);
  if (!blobp) return false;
  const std::vector<uint8_t>& b = *blobp;

  unwind_info_section_header hdr = {};
  if (!pick(b, 0, &hdr, sizeof(hdr)) || hdr.version != UNWIND_SECTION_VERSION) return false;

  uint32_t pcOff = (uint32_t)(pc - im->loadAddr); // index は mach_header 基準のオフセット

  // 第 1 レベル索引を線形走査（要素数は少ない）。index[i] <= pcOff < index[i+1] を探す。
  uint32_t idxOff = hdr.indexSectionOffset, n = hdr.indexCount;
  if (n < 2) return false;
  unwind_info_section_header_index_entry e = {}, next = {};
  bool found = false;
  for (uint32_t i = 0; i + 1 < n; i++) {
    if (!pick(b, idxOff + i * sizeof(e), &e, sizeof(e))) return false;
    if (!pick(b, idxOff + (i + 1) * sizeof(next), &next, sizeof(next))) return false;
    if (pcOff >= e.functionOffset && pcOff < next.functionOffset) { found = true; break; }
  }
  if (!found) return false;

  uint32_t pageOff = e.secondLevelPagesSectionOffset;
  if (!pageOff) return false;
  uint32_t kind = 0;
  if (!pick(b, pageOff, &kind, sizeof(kind))) return false;

  if (kind == UNWIND_SECOND_LEVEL_REGULAR) {
    unwind_info_regular_second_level_page_header ph = {};
    if (!pick(b, pageOff, &ph, sizeof(ph))) return false;
    uint32_t entriesOff = pageOff + ph.entryPageOffset;
    // 最大の functionOffset <= pcOff を探す（昇順）。
    unwind_info_regular_second_level_entry ent = {}, best = {};
    bool any = false;
    for (uint16_t i = 0; i < ph.entryCount; i++) {
      if (!pick(b, entriesOff + i * sizeof(ent), &ent, sizeof(ent))) return false;
      if (ent.functionOffset <= pcOff) { best = ent; any = true; } else break;
    }
    if (!any) return false;
    encoding = best.encoding;
    funcStart = im->loadAddr + best.functionOffset;
    return true;
  }

  if (kind == UNWIND_SECOND_LEVEL_COMPRESSED) {
    unwind_info_compressed_second_level_page_header ph = {};
    if (!pick(b, pageOff, &ph, sizeof(ph))) return false;
    uint32_t entriesOff = pageOff + ph.entryPageOffset;
    uint32_t firstFunc = e.functionOffset; // このページの基準
    uint32_t bestEntry = 0; bool any = false; uint32_t bestFuncOff = 0;
    for (uint16_t i = 0; i < ph.entryCount; i++) {
      uint32_t raw = 0;
      if (!pick(b, entriesOff + i * sizeof(uint32_t), &raw, sizeof(raw))) return false;
      uint32_t fo = firstFunc + UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(raw);
      if (fo <= pcOff) { bestEntry = raw; bestFuncOff = fo; any = true; } else break;
    }
    if (!any) return false;
    uint32_t encIdx = UNWIND_INFO_COMPRESSED_ENTRY_ENCODING_INDEX(bestEntry);
    if (encIdx < hdr.commonEncodingsArrayCount) {
      if (!pick(b, hdr.commonEncodingsArraySectionOffset + encIdx * sizeof(uint32_t),
                &encoding, sizeof(encoding))) return false;
    } else {
      uint32_t local = encIdx - hdr.commonEncodingsArrayCount;
      uint32_t encArrOff = pageOff + ph.encodingsPageOffset;
      if (local >= ph.encodingsCount) return false;
      if (!pick(b, encArrOff + local * sizeof(uint32_t), &encoding, sizeof(encoding))) return false;
    }
    funcStart = im->loadAddr + bestFuncOff;
    return true;
  }

  return false; // 未知のページ種別
}

const std::vector<uint8_t>* Unwinder::ehFor(const Image& im) {
  uint64_t addr = im.ehFrameAddr();
  if (!addr || !im.ehframeSize) return nullptr;
  auto it = ehCache_.find(im.loadAddr);
  if (it != ehCache_.end()) return it->second.empty() ? nullptr : &it->second;
  std::vector<uint8_t> buf(im.ehframeSize);
  if (!read_(addr, buf.data(), buf.size())) buf.clear();
  auto& stored = ehCache_[im.loadAddr] = std::move(buf);
  return stored.empty() ? nullptr : &stored;
}

namespace {
// __eh_frame を読むためのバイトカーソル（範囲外は ok_=false にして以後の読みを無害化）。
struct Cur {
  const uint8_t* p; const uint8_t* end; bool ok = true;
  uint8_t u8()  { if (p + 1 > end) { ok = false; return 0; } return *p++; }
  uint16_t u16(){ uint16_t v=0; if(p+2>end){ok=false;return 0;} memcpy(&v,p,2); p+=2; return v; }
  uint32_t u32(){ uint32_t v=0; if(p+4>end){ok=false;return 0;} memcpy(&v,p,4); p+=4; return v; }
  uint64_t u64(){ uint64_t v=0; if(p+8>end){ok=false;return 0;} memcpy(&v,p,8); p+=8; return v; }
  uint64_t uleb(){ uint64_t r=0; int s=0; for(;;){ if(p>=end){ok=false;break;} uint8_t b=*p++; r|=(uint64_t)(b&0x7f)<<s; if(!(b&0x80))break; s+=7; } return r; }
  int64_t sleb(){ int64_t r=0; int s=0; uint8_t b=0; do{ if(p>=end){ok=false;break;} b=*p++; r|=(int64_t)(b&0x7f)<<s; s+=7; }while(b&0x80); if(s<64 && (b&0x40)) r|=-((int64_t)1<<s); return r; }
};

// DWARF EH ポインタエンコーディング
enum { PE_omit=0xff, PE_absptr=0x00, PE_uleb=0x01, PE_udata2=0x02, PE_udata4=0x03, PE_udata8=0x04,
       PE_sleb=0x09, PE_sdata2=0x0a, PE_sdata4=0x0b, PE_sdata8=0x0c };

// enc 形式のポインタを読む。fieldRuntime はこのフィールドの実行時アドレス（pcrel 用）。
static uint64_t readEncPtr(Cur& c, uint8_t enc, uint64_t fieldRuntime) {
  if (enc == PE_omit) return 0;
  uint64_t base = 0;
  if ((enc & 0x70) == 0x10) base = fieldRuntime; // DW_EH_PE_pcrel
  uint64_t v = 0;
  switch (enc & 0x0f) {
    case PE_absptr: v = c.u64(); break;
    case PE_uleb:   v = c.uleb(); break;
    case PE_udata2: v = c.u16(); break;
    case PE_udata4: v = c.u32(); break;
    case PE_udata8: v = c.u64(); break;
    case PE_sleb:   v = (uint64_t)c.sleb(); break;
    case PE_sdata2: v = (uint64_t)(int64_t)(int16_t)c.u16(); break;
    case PE_sdata4: v = (uint64_t)(int64_t)(int32_t)c.u32(); break;
    case PE_sdata8: v = c.u64(); break;
    default: c.ok = false; return 0;
  }
  return base + v;
}

// CFI 解釈で追う最小の状態。regRule: 0=未定義/不変, 1=CFA+offset に退避。
struct Rules {
  int cfaReg = -1; int64_t cfaOff = 0;
  static const int N = 34;
  uint8_t kind[N] = {0};   // 0=none, 1=offset(CFA)
  int64_t off[N] = {0};
};
} // namespace

// __eh_frame の FDE を解釈して 1 段アンワインドする。
bool Unwinder::stepDwarf(const Image& im, uint32_t fdeOff, const UnwindRegs& in, UnwindRegs& out) {
  const std::vector<uint8_t>* ehp = ehFor(im);
  if (!ehp) return false;
  const std::vector<uint8_t>& b = *ehp;
  uint64_t ehBase = im.ehFrameAddr();
  if (fdeOff + 4 > b.size()) return false;

  // --- FDE ヘッダ ---
  Cur f{ b.data() + fdeOff, b.data() + b.size() };
  uint32_t fdeLen = f.u32();
  if (fdeLen == 0 || fdeLen == 0xffffffff) return false; // 64bit 長は非対応
  const uint8_t* fdeEnd = f.p + fdeLen;
  if (fdeEnd > b.data() + b.size()) return false;
  uint64_t ciePtrFieldOff = (uint64_t)(f.p - b.data());
  uint32_t cieBack = f.u32();
  if (cieBack == 0) return false;                 // これは CIE（FDE ではない）
  uint64_t cieStartOff = ciePtrFieldOff - cieBack; // .eh_frame の CIE 逆参照

  // --- CIE ---
  if (cieStartOff + 4 > b.size()) return false;
  Cur c{ b.data() + cieStartOff, b.data() + b.size() };
  uint32_t cieLen = c.u32();
  const uint8_t* cieEnd = c.p + cieLen;
  if (cieEnd > b.data() + b.size()) return false;
  if (c.u32() != 0) return false;                 // CIE id は 0
  uint8_t version = c.u8();
  std::string aug;
  while (c.ok) { uint8_t ch = c.u8(); if (!ch) break; aug.push_back((char)ch); }
  uint64_t caf = c.uleb();
  int64_t daf = c.sleb();
  int raReg = (version == 1) ? c.u8() : (int)c.uleb();
  uint8_t fdeEnc = PE_absptr;
  if (!aug.empty() && aug[0] == 'z') {
    uint64_t al = c.uleb(); const uint8_t* augEnd = c.p + al;
    for (size_t i = 1; i < aug.size(); i++) {
      if (aug[i] == 'R') fdeEnc = c.u8();
      else if (aug[i] == 'P') { uint8_t pe = c.u8(); uint64_t fr = ehBase + (c.p - b.data()); readEncPtr(c, pe, fr); }
      else if (aug[i] == 'L') c.u8();
    }
    c.p = augEnd; // 残りのアグメンテーションデータを読み飛ばす
  }
  const uint8_t* cieInstr = c.p;
  if (!c.ok) return false;

  // --- FDE の pc_begin / pc_range（エンコーディングは CIE の 'R'） ---
  uint64_t pcBeginField = ehBase + (f.p - b.data());
  uint64_t pcBegin = readEncPtr(f, fdeEnc, pcBeginField);
  uint64_t pcRange = readEncPtr(f, (uint8_t)(fdeEnc & 0x0f), 0); // range は値のみ（application 無し）
  if (!aug.empty() && aug[0] == 'z') { uint64_t al = f.uleb(); f.p += al; } // FDE アグメンテーション
  const uint8_t* fdeInstr = f.p;
  if (!f.ok) return false;
  (void)pcRange;

  // --- CFI プログラム実行（CIE 初期命令 → FDE 命令。location<=in.pc まで） ---
  Rules R; std::vector<Rules> stack;
  uint64_t loc = pcBegin;
  auto run = [&](Cur cc, const uint8_t* stop) -> bool {
    while (cc.p < stop && cc.ok) {
      uint8_t op = cc.u8(); uint8_t hi = op & 0xc0, lo = op & 0x3f;
      if (hi == 0x40) { uint64_t nl = loc + (uint64_t)lo * caf; if (nl > in.pc) return true; loc = nl; }
      else if (hi == 0x80) { uint64_t off = cc.uleb(); if (lo < Rules::N) { R.kind[lo]=1; R.off[lo]=(int64_t)off*daf; } }
      else if (hi == 0xc0) { /* DW_CFA_restore: 近似的に none へ */ if (lo < Rules::N) R.kind[lo]=0; }
      else switch (op) {
        case 0x00: break;                                   // nop
        case 0x01: { uint64_t fr=ehBase+(cc.p-b.data()); uint64_t nl=readEncPtr(cc,fdeEnc,fr); if(nl>in.pc) return true; loc=nl; } break; // set_loc
        case 0x02: { uint64_t nl=loc+(uint64_t)cc.u8()*caf; if(nl>in.pc) return true; loc=nl; } break; // advance_loc1
        case 0x03: { uint64_t nl=loc+(uint64_t)cc.u16()*caf; if(nl>in.pc) return true; loc=nl; } break;
        case 0x04: { uint64_t nl=loc+(uint64_t)cc.u32()*caf; if(nl>in.pc) return true; loc=nl; } break;
        case 0x05: { uint64_t r=cc.uleb(); uint64_t o=cc.uleb(); if(r<Rules::N){R.kind[r]=1;R.off[r]=(int64_t)o*daf;} } break; // offset_extended
        case 0x06: { uint64_t r=cc.uleb(); if(r<Rules::N)R.kind[r]=0; } break;        // restore_extended
        case 0x07: { uint64_t r=cc.uleb(); if(r<Rules::N)R.kind[r]=0; } break;        // undefined
        case 0x08: { uint64_t r=cc.uleb(); if(r<Rules::N)R.kind[r]=0; } break;        // same_value
        case 0x09: { cc.uleb(); cc.uleb(); } break;                                    // register（非対応→不変扱い）
        case 0x0a: stack.push_back(R); break;                                          // remember_state
        case 0x0b: if(!stack.empty()){R=stack.back();stack.pop_back();} break;         // restore_state
        case 0x0c: { R.cfaReg=(int)cc.uleb(); R.cfaOff=(int64_t)cc.uleb(); } break;    // def_cfa
        case 0x0d: R.cfaReg=(int)cc.uleb(); break;                                     // def_cfa_register
        case 0x0e: R.cfaOff=(int64_t)cc.uleb(); break;                                 // def_cfa_offset
        case 0x0f: return false;                                                       // def_cfa_expression（非対応）
        case 0x10: { cc.uleb(); uint64_t l=cc.uleb(); cc.p+=l; } break;                // expression（非対応→不変）
        case 0x11: { uint64_t r=cc.uleb(); int64_t o=cc.sleb(); if(r<Rules::N){R.kind[r]=1;R.off[r]=o*daf;} } break; // offset_extended_sf
        case 0x12: { R.cfaReg=(int)cc.uleb(); R.cfaOff=cc.sleb()*(int64_t)daf; } break;// def_cfa_sf
        case 0x13: R.cfaOff=cc.sleb()*(int64_t)daf; break;                             // def_cfa_offset_sf
        case 0x2e: cc.uleb(); break;                                                   // GNU_args_size
        default: return false;                                                         // 未対応オペコード
      }
    }
    return true;
  };
  if (!run(Cur{cieInstr, cieEnd}, cieEnd)) return false;
  if (!run(Cur{fdeInstr, fdeEnd}, fdeEnd)) return false;

  // --- CFA と戻りアドレス・fp を算出 ---
  if (R.cfaReg < 0) return false;
  uint64_t cfaBase;
  if (R.cfaReg == arch_.dwarfRegSP()) cfaBase = in.sp;
  else if (R.cfaReg == arch_.dwarfRegFP()) cfaBase = in.fp;
  else return false; // sp/fp 以外を基底にする CFA は非対応
  uint64_t cfa = cfaBase + (uint64_t)R.cfaOff;

  // 戻りアドレス（CIE が宣言する return_address_register の規則で復元）
  uint64_t ra = 0;
  int rr = raReg;
  if (rr < Rules::N && R.kind[rr] == 1) {
    if (!read_(cfa + (uint64_t)R.off[rr], &ra, 8)) return false;
  } else {
    ra = in.lr; // 退避規則なし（leaf）＝戻りアドレスは LR に残っている
  }
  if (!ra) return false;

  // 呼び出し元 fp（保存されていれば復元、無ければ据え置き）
  uint64_t fp = in.fp;
  int fr = arch_.dwarfRegFP();
  if (fr < Rules::N && R.kind[fr] == 1) { uint64_t v=0; if (read_(cfa + (uint64_t)R.off[fr], &v, 8)) fp = v; }

  out.pc = ra; out.sp = cfa; out.fp = fp; out.lr = 0;
  return true;
}

bool Unwinder::step(const UnwindRegs& in, UnwindRegs& out) {
  uint32_t enc = 0; uint64_t funcStart = 0;
  if (!encodingFor(in.pc, enc, funcStart)) return false;
  if (arch_.unwindIsDwarf(enc)) {
    const Image* im = images_.byAddress(in.pc);
    if (!im) return false;
    return stepDwarf(*im, arch_.unwindDwarfOffset(enc), in, out);
  }
  return arch_.stepUnwind(enc, funcStart, in, out, read_);
}

} // namespace ta2
