#include "core/DwarfTypes.h"

#include <cstring>

namespace ta2 {
namespace {

// ---- バイト列リーダ（境界チェック付き。DwarfLines.cpp と同型） ----
struct Rd {
  const uint8_t* base;  // セクション先頭（絶対オフセット算出用）
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  uint64_t off() const { return (uint64_t)(p - base); }
  uint8_t u8() { if (p >= end) { ok = false; return 0; } return *p++; }
  uint16_t u16() { uint16_t v = u8(); v |= (uint16_t)u8() << 8; return v; }
  uint32_t u32() { uint32_t v = u16(); v |= (uint32_t)u16() << 16; return v; }
  uint64_t u64() { uint64_t v = u32(); v |= (uint64_t)u32() << 32; return v; }
  uint64_t uleb() { uint64_t r = 0; int s = 0; for (;;) { uint8_t b = u8(); if (!ok) break; r |= (uint64_t)(b & 0x7f) << s; if (!(b & 0x80)) break; s += 7; } return r; }
  int64_t sleb() { int64_t r = 0; int s = 0; uint8_t b = 0; for (;;) { b = u8(); if (!ok) break; r |= (int64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; } if (s < 64 && (b & 0x40)) r |= -((int64_t)1 << s); return r; }
  std::string cstr() { std::string s; for (;;) { uint8_t b = u8(); if (!ok || b == 0) break; s += (char)b; } return s; }
  void skip(size_t n) { if (p + n > end) { p = end; ok = false; } else p += n; }
};

// ---- .debug_abbrev: code -> (tag, children, (attr,form,implicit) 列) ----
struct AbbrevAttr { uint64_t attr, form; int64_t implicit; };
struct Abbrev { uint64_t tag; bool children; std::vector<AbbrevAttr> attrs; };

std::unordered_map<uint64_t, Abbrev> parseAbbrev(const std::vector<uint8_t>& ab, uint64_t offset) {
  std::unordered_map<uint64_t, Abbrev> m;
  if (offset >= ab.size()) return m;
  Rd r{ab.data(), ab.data() + offset, ab.data() + ab.size()};
  for (;;) {
    uint64_t code = r.uleb();
    if (!r.ok || code == 0) break;
    Abbrev a;
    a.tag = r.uleb();
    a.children = (r.u8() != 0);
    for (;;) {
      uint64_t at = r.uleb(), fm = r.uleb();
      int64_t ic = 0;
      if (fm == 0x21 /*implicit_const*/) ic = r.sleb();
      if (at == 0 && fm == 0) break;
      a.attrs.push_back({at, fm, ic});
    }
    if (!r.ok) break;
    m[code] = std::move(a);
  }
  return m;
}

// DWARF タグ / 属性 / form / 基本型エンコーディングの定数（使うものだけ）。
enum {
  TAG_array = 0x01, TAG_class = 0x02, TAG_enumeration = 0x04, TAG_member = 0x0d,
  TAG_pointer = 0x0f, TAG_compile_unit = 0x11, TAG_structure = 0x13, TAG_subroutine = 0x15,
  TAG_typedef = 0x16, TAG_union = 0x17, TAG_base = 0x24, TAG_const = 0x26,
  TAG_enumerator = 0x28, TAG_subrange = 0x21, TAG_volatile = 0x35,
};
enum {
  AT_name = 0x03, AT_byte_size = 0x0b, AT_bit_offset = 0x0c, AT_bit_size = 0x0d,
  AT_stmt_list = 0x10, AT_encoding = 0x3e, AT_type = 0x49, AT_data_member_location = 0x38,
  AT_const_value = 0x1c, AT_upper_bound = 0x2f, AT_count = 0x37, AT_declaration = 0x3c,
  AT_data_bit_offset = 0x6b,
};

// 1 つの属性値。kind: 0=なし,1=文字列,2=数値,3=参照(絶対オフセット),4=block(bytes)。
struct FormVal {
  int kind = 0;
  std::string s;
  uint64_t n = 0;       // 数値 or 参照オフセット
  int64_t sn = 0;       // 符号付き数値（const_value 等）
  std::vector<uint8_t> block;
};

// form を読み、必要な値（文字列/数値/参照オフセット/block）を取り出す。cuStart は参照を絶対化する基準。
void readForm(Rd& r, uint64_t form, int addrSize, uint64_t cuStart, int64_t implicit,
              const std::vector<uint8_t>& str, const std::vector<uint8_t>& lineStr, FormVal& out) {
  out = FormVal{};
  switch (form) {
    case 0x01: out.n = (addrSize == 8) ? r.u64() : r.u32(); out.kind = 2; break; // addr
    case 0x08: out.s = r.cstr(); out.kind = 1; break;                             // string(inline)
    case 0x0e: { uint32_t o = r.u32(); if (o < str.size()) { out.s = (const char*)str.data() + o; out.kind = 1; } break; } // strp
    case 0x1f: { uint32_t o = r.u32(); if (o < lineStr.size()) { out.s = (const char*)lineStr.data() + o; out.kind = 1; } break; } // line_strp
    case 0x0b: out.n = r.u8();  out.sn = (int8_t)out.n;  out.kind = 2; break;      // data1
    case 0x05: out.n = r.u16(); out.sn = (int16_t)out.n; out.kind = 2; break;      // data2
    case 0x06: out.n = r.u32(); out.sn = (int32_t)out.n; out.kind = 2; break;      // data4
    case 0x07: out.n = r.u64(); out.sn = (int64_t)out.n; out.kind = 2; break;      // data8
    case 0x1e: r.skip(16); break;                                                 // data16
    case 0x0d: out.sn = r.sleb(); out.n = (uint64_t)out.sn; out.kind = 2; break;  // sdata
    case 0x0f: out.n = r.uleb(); out.sn = (int64_t)out.n; out.kind = 2; break;    // udata
    case 0x0c: out.n = r.u8(); out.kind = 2; break;                               // flag
    case 0x19: out.n = 1; out.kind = 2; break;                                    // flag_present
    case 0x17: out.n = r.u32(); out.kind = 2; break;                              // sec_offset(DWARF32)
    case 0x11: out.n = cuStart + r.u8();  out.kind = 3; break;                     // ref1
    case 0x12: out.n = cuStart + r.u16(); out.kind = 3; break;                     // ref2
    case 0x13: out.n = cuStart + r.u32(); out.kind = 3; break;                     // ref4
    case 0x14: out.n = cuStart + r.u64(); out.kind = 3; break;                     // ref8
    case 0x15: out.n = cuStart + r.uleb(); out.kind = 3; break;                    // ref_udata
    case 0x10: out.n = r.u32(); out.kind = 3; break;                              // ref_addr(DWARF32, 絶対)
    case 0x18: { uint64_t n = r.uleb(); out.block.resize(n); for (uint64_t i = 0; i < n && r.ok; i++) out.block[i] = r.u8(); out.kind = 4; break; } // exprloc
    case 0x09: { uint64_t n = r.uleb(); out.block.resize(n); for (uint64_t i = 0; i < n && r.ok; i++) out.block[i] = r.u8(); out.kind = 4; break; } // block
    case 0x0a: { uint64_t n = r.u8();  out.block.resize(n); for (uint64_t i = 0; i < n && r.ok; i++) out.block[i] = r.u8(); out.kind = 4; break; } // block1
    case 0x03: { uint64_t n = r.u16(); out.block.resize(n); for (uint64_t i = 0; i < n && r.ok; i++) out.block[i] = r.u8(); out.kind = 4; break; } // block2
    case 0x04: { uint64_t n = r.u32(); out.block.resize(n); for (uint64_t i = 0; i < n && r.ok; i++) out.block[i] = r.u8(); out.kind = 4; break; } // block4
    case 0x20: r.skip(8); break;                                                  // ref_sig8
    case 0x21: out.sn = implicit; out.n = (uint64_t)implicit; out.kind = 2; break; // implicit_const（値は abbrev 側）
    // DWARF5 の indexed 文字列/アドレス。str_offsets/addr 表を読んでいないので名前は解決できない（飛ばす）。
    case 0x1a: r.uleb(); break;                                                   // strx
    case 0x25: r.skip(1); break;                                                  // strx1
    case 0x26: r.skip(2); break;                                                  // strx2
    case 0x27: r.skip(3); break;                                                  // strx3
    case 0x28: r.skip(4); break;                                                  // strx4
    case 0x1b: r.uleb(); break;                                                   // addrx
    default: r.ok = false; break;                                                // 未知フォーム → 中断
  }
}

// data_member_location が exprloc のとき、DW_OP_plus_uconst(0x23) <uleb> だけ解釈してオフセットを得る。
bool exprPlusUconst(const std::vector<uint8_t>& blk, uint64_t& off) {
  if (blk.empty() || blk[0] != 0x23) return false;
  uint64_t r = 0; int s = 0; size_t i = 1;
  for (; i < blk.size(); i++) { r |= (uint64_t)(blk[i] & 0x7f) << s; if (!(blk[i] & 0x80)) { off = r; return true; } s += 7; }
  return false;
}

} // namespace

// 1 つのセクション群（.debug_info/.debug_abbrev/...）から型を取り込む。
void DwarfTypes::addSections(const DwarfSections& s) {
  if (s.info.empty() || s.abbrev.empty()) return;
  int sec = (int)sections_.size();
  sections_.emplace_back();
  auto& tmap = sections_.back();

  Rd r{s.info.data(), s.info.data(), s.info.data() + s.info.size()};
  while (r.ok && r.p < r.end) {
    uint64_t cuStart = r.off();          // この CU の先頭（ref* の基準）
    uint32_t len = r.u32();
    if (len == 0xffffffffu || len == 0) break; // DWARF64/不正は非対応
    const uint8_t* cuEnd = r.p + len;
    if (cuEnd > r.end) break;
    uint16_t ver = r.u16();
    uint64_t abbrevOff; int addrSize;
    if (ver >= 5) { r.u8(); addrSize = r.u8(); abbrevOff = r.u32(); } // unit_type, addr_size, abbrev_off
    else { abbrevOff = r.u32(); addrSize = r.u8(); }
    auto ab = parseAbbrev(s.abbrev, abbrevOff);

    // DIE を平坦走査。children を持つ DIE は子が続き、code=0 で兄弟列が終わる（親スタックで追う）。
    std::vector<uint64_t> parentTag;  // スタック: 子を持つ DIE の tag
    std::vector<uint64_t> parentOff;  // 同: その DIE の絶対オフセット
    while (r.ok && r.p < cuEnd) {
      uint64_t dieOff = r.off();
      uint64_t code = r.uleb();
      if (code == 0) {                 // 兄弟列の終端 → 1 段戻る
        if (!parentTag.empty()) { parentTag.pop_back(); parentOff.pop_back(); }
        continue;
      }
      auto it = ab.find(code);
      if (it == ab.end()) { r.ok = false; break; } // 未知 abbrev → この CU は諦める
      const Abbrev& a = it->second;

      // 属性を読み取る
      std::string name; uint64_t byteSize = 0; bool haveByteSize = false;
      int encoding = 0; DwTypeRef typeRef; uint64_t memberOff = 0; bool haveMemberOff = false;
      int64_t constVal = 0; bool haveConst = false; uint64_t upper = 0; bool haveUpper = false, haveCount = false;
      uint64_t countVal = 0; bool declOnly = false; int bitSize = 0; bool haveBitSize = false;
      int dataBitOff = 0; bool haveDataBitOff = false;
      for (const auto& at : a.attrs) {
        FormVal v;
        readForm(r, at.form, addrSize, cuStart, at.implicit, s.str, s.lineStr, v);
        if (!r.ok) break;
        switch (at.attr) {
          case AT_name: if (v.kind == 1) name = v.s; break;
          case AT_byte_size: if (v.kind == 2) { byteSize = v.n; haveByteSize = true; } break;
          case AT_encoding: if (v.kind == 2) encoding = (int)v.n; break;
          case AT_type: if (v.kind == 3) { typeRef = DwTypeRef{sec, v.n, true}; } break;
          case AT_data_member_location:
            if (v.kind == 2) { memberOff = v.n; haveMemberOff = true; }
            else if (v.kind == 4) { uint64_t o; if (exprPlusUconst(v.block, o)) { memberOff = o; haveMemberOff = true; } }
            break;
          case AT_const_value: if (v.kind == 2) { constVal = v.sn; haveConst = true; } break;
          case AT_upper_bound: if (v.kind == 2) { upper = v.n; haveUpper = true; } break;
          case AT_count: if (v.kind == 2) { countVal = v.n; haveCount = true; } break;
          case AT_declaration: if (v.kind == 2 && v.n) declOnly = true; break;
          case AT_bit_size: if (v.kind == 2) { bitSize = (int)v.n; haveBitSize = true; } break;
          case AT_data_bit_offset: if (v.kind == 2) { dataBitOff = (int)v.n; haveDataBitOff = true; } break;
          default: break;
        }
      }
      if (!r.ok) break;

      uint64_t curParentTag = parentTag.empty() ? 0 : parentTag.back();
      uint64_t curParentOff = parentOff.empty() ? 0 : parentOff.back();

      // 子（メンバ/列挙子/配列長）を親へ結びつける
      if (a.tag == TAG_member &&
          (curParentTag == TAG_structure || curParentTag == TAG_union || curParentTag == TAG_class)) {
        auto pit = tmap.find(curParentOff);
        if (pit != tmap.end()) {
          DwMember m; m.name = name; m.offset = haveMemberOff ? memberOff : 0; m.type = typeRef;
          if (haveBitSize) { m.isBit = true; m.bitSize = bitSize; m.bitOffset = haveDataBitOff ? dataBitOff : 0; }
          pit->second.members.push_back(std::move(m));
        }
      } else if (a.tag == TAG_enumerator && curParentTag == TAG_enumeration) {
        auto pit = tmap.find(curParentOff);
        if (pit != tmap.end() && haveConst) pit->second.enums.push_back(DwEnumerator{name, constVal});
      } else if (a.tag == TAG_subrange && curParentTag == TAG_array) {
        auto pit = tmap.find(curParentOff);
        if (pit != tmap.end()) {
          uint64_t cnt = haveCount ? countVal : (haveUpper ? upper + 1 : 0);
          pit->second.arrayCount = cnt;
        }
      }

      // 型 DIE を記録（declaration だけの前方宣言は実体が無いので記録しない）
      DwType::Kind kind = DwType::Unknown;
      switch (a.tag) {
        case TAG_structure: kind = DwType::Struct; break;
        case TAG_union: kind = DwType::Union; break;
        case TAG_class: kind = DwType::Class; break;
        case TAG_base: kind = DwType::Base; break;
        case TAG_typedef: kind = DwType::Typedef; break;
        case TAG_pointer: kind = DwType::Pointer; break;
        case TAG_const: kind = DwType::Const; break;
        case TAG_volatile: kind = DwType::Volatile; break;
        case TAG_array: kind = DwType::Array; break;
        case TAG_enumeration: kind = DwType::Enum; break;
        default: break;
      }
      if (kind != DwType::Unknown && !declOnly) {
        DwType t; t.kind = kind; t.name = name; t.encoding = encoding; t.ref = typeRef; t.section = sec;
        if (haveByteSize) t.byteSize = byteSize;
        if (kind == DwType::Pointer && !haveByteSize) t.byteSize = addrSize ? addrSize : 8;
        tmap[dieOff] = std::move(t);
        // 名前つきの集約型/enum/typedef/基本型は名前索引へ（先勝ち）
        if (!name.empty() && (kind == DwType::Struct || kind == DwType::Union || kind == DwType::Class ||
                              kind == DwType::Enum || kind == DwType::Typedef || kind == DwType::Base))
          byName_.emplace(name, DwTypeRef{sec, dieOff, true});
      }

      if (a.children) { parentTag.push_back(a.tag); parentOff.push_back(dieOff); }
    }
    r.p = cuEnd; // 次の CU へ
    r.ok = true; // 1 CU の失敗は他 CU に波及させない
  }
}

const DwType* DwarfTypes::at(const DwTypeRef& ref) const {
  if (!ref.valid || ref.section < 0 || ref.section >= (int)sections_.size()) return nullptr;
  const auto& m = sections_[ref.section];
  auto it = m.find(ref.off);
  return it == m.end() ? nullptr : &it->second;
}

const DwType* DwarfTypes::byName(const std::string& name) const {
  auto it = byName_.find(name);
  if (it == byName_.end()) return nullptr;
  return at(it->second);
}

void DwarfTypes::names(std::vector<std::string>& out) const {
  out.clear();
  out.reserve(byName_.size());
  for (const auto& kv : byName_) out.push_back(kv.first);
}

} // namespace ta2
