#include "core/DwarfLines.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>

namespace ta2 {

// __debug セクション群（必要なものだけ）。line_str は DWARF5 の行テーブル用。
// dSYM（__DWARF セグメント）と .o（無名セグメント内の __DWARF セクション）の両方から読む。
struct DwarfSections {
  std::vector<uint8_t> line, info, abbrev, str, lineStr;
};

namespace {

// ---- バイト列リーダ（境界チェック付き）----
struct Rd {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  uint8_t u8() { if (p >= end) { ok = false; return 0; } return *p++; }
  uint16_t u16() { uint16_t v = u8(); v |= (uint16_t)u8() << 8; return v; }
  uint32_t u32() { uint32_t v = u16(); v |= (uint32_t)u16() << 16; return v; }
  uint64_t u64() { uint64_t v = u32(); v |= (uint64_t)u32() << 32; return v; }
  uint64_t uleb() { uint64_t r = 0; int s = 0; for (;;) { uint8_t b = u8(); if (!ok) break; r |= (uint64_t)(b & 0x7f) << s; if (!(b & 0x80)) break; s += 7; } return r; }
  int64_t sleb() { int64_t r = 0; int s = 0; uint8_t b = 0; for (;;) { b = u8(); if (!ok) break; r |= (int64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; } if (s < 64 && (b & 0x40)) r |= -((int64_t)1 << s); return r; }
  std::string cstr() { std::string s; for (;;) { uint8_t b = u8(); if (!ok || b == 0) break; s += (char)b; } return s; }
  void skip(size_t n) { if (p + n > end) { p = end; ok = false; } else p += n; }
};

// Mach-O(64) スライスから __DWARF の各セクションを読む。
// wantUuid!=nullptr のときは LC_UUID 一致を要求する（dSYM 用）。nullptr なら要求しない（.o 用）。
// __DWARF の判定は「セクション側の segname」で行う: dSYM はセクションが __DWARF セグメントに
// 属するが、.o（MH_OBJECT）は全セクションが無名セグメントに入り、各セクションの segname が
// __DWARF になっている。どちらもセクション単位で見れば拾える。
bool readDwarfSlice(FILE* f, uint64_t off, const uint8_t* wantUuid, DwarfSections& out) {
  struct mach_header_64 mh;
  if (fseeko(f, (off_t)off, SEEK_SET) != 0 || fread(&mh, sizeof(mh), 1, f) != 1) return false;
  if (mh.magic != MH_MAGIC_64) return false;
  std::vector<uint8_t> lc(mh.sizeofcmds);
  if (fread(lc.data(), 1, mh.sizeofcmds, f) != mh.sizeofcmds) return false;

  bool uuidOk = (wantUuid == nullptr);
  struct Sec { uint64_t fileoff; uint64_t size; std::vector<uint8_t>* dst; };
  std::vector<Sec> want;
  const uint8_t* p = lc.data(); const uint8_t* end = p + mh.sizeofcmds;
  for (uint32_t i = 0; i < mh.ncmds && p + sizeof(load_command) <= end; i++) {
    auto* c = reinterpret_cast<const load_command*>(p);
    if (c->cmdsize < sizeof(load_command) || p + c->cmdsize > end) break;
    if (c->cmd == LC_UUID) {
      if (wantUuid) uuidOk = (memcmp(reinterpret_cast<const uuid_command*>(p)->uuid, wantUuid, 16) == 0);
    } else if (c->cmd == LC_SEGMENT_64) {
      auto* sg = reinterpret_cast<const segment_command_64*>(p);
      auto* s = reinterpret_cast<const section_64*>(p + sizeof(segment_command_64));
      for (uint32_t j = 0; j < sg->nsects; j++, s++) {
        if (strncmp(s->segname, "__DWARF", 16) != 0) continue; // __DWARF セクションだけ見る
        std::vector<uint8_t>* dst = nullptr;
        // __debug_line_str（DWARF5 の行テーブル文字列）はちょうど 16 文字で char[16] に終端なく
        // 収まる。__debug_line より先に見る（strncmp は NUL 止まりなので誤判定はしないが明示）。
        if (!strncmp(s->sectname, "__debug_line_str", 16)) dst = &out.lineStr;
        else if (!strncmp(s->sectname, "__debug_line", 16)) dst = &out.line;
        else if (!strncmp(s->sectname, "__debug_info", 16)) dst = &out.info;
        else if (!strncmp(s->sectname, "__debug_abbrev", 16)) dst = &out.abbrev;
        else if (!strncmp(s->sectname, "__debug_str", 16)) dst = &out.str;
        if (dst) want.push_back({s->offset, s->size, dst});
      }
    }
    p += c->cmdsize;
  }
  if (!uuidOk) return false;
  for (auto& w : want) {
    w.dst->resize(w.size);
    if (w.size && (fseeko(f, (off_t)(off + w.fileoff), SEEK_SET) != 0 || fread(w.dst->data(), 1, w.size, f) != w.size)) { w.dst->clear(); }
  }
  return !out.line.empty();
}

// path（dSYM または .o）から __DWARF セクションを読む。thin / fat 両対応。
bool loadDwarfSections(const std::string& path, const uint8_t* uuid, DwarfSections& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = false;
  uint32_t magic = 0;
  if (fread(&magic, 4, 1, f) == 1) {
    if (magic == MH_MAGIC_64) {
      ok = readDwarfSlice(f, 0, uuid, out);
    } else if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
      bool swap = (magic == FAT_CIGAM);
      uint32_t nfat = 0;
      if (fread(&nfat, 4, 1, f) == 1) {
        if (swap) nfat = __builtin_bswap32(nfat);
        for (uint32_t i = 0; i < nfat && i < 32; i++) {
          struct fat_arch fa;
          if (fread(&fa, sizeof(fa), 1, f) != 1) break;
          uint64_t sliceOff = swap ? __builtin_bswap32(fa.offset) : fa.offset;
          long save = ftell(f);
          if (readDwarfSlice(f, sliceOff, uuid, out)) { ok = true; break; }
          fseek(f, save, SEEK_SET);
        }
      }
    }
  }
  fclose(f);
  return ok;
}

// ---- LC_SYMTAB（nlist + 文字列表）の読み出し ----
// 本体の debug map（STABS）と .o のシンボル（objAddr）をファイルから読むために使う。
struct NList { uint8_t type; uint8_t sect; uint16_t desc; uint64_t value; std::string name; };

bool readSymtabSlice(FILE* f, uint64_t off, const uint8_t* wantUuid, std::vector<NList>& out) {
  struct mach_header_64 mh;
  if (fseeko(f, (off_t)off, SEEK_SET) != 0 || fread(&mh, sizeof(mh), 1, f) != 1) return false;
  if (mh.magic != MH_MAGIC_64) return false;
  std::vector<uint8_t> lc(mh.sizeofcmds);
  if (fread(lc.data(), 1, mh.sizeofcmds, f) != mh.sizeofcmds) return false;

  bool uuidOk = (wantUuid == nullptr);
  uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0; bool haveSym = false;
  const uint8_t* p = lc.data(); const uint8_t* end = p + mh.sizeofcmds;
  for (uint32_t i = 0; i < mh.ncmds && p + sizeof(load_command) <= end; i++) {
    auto* c = reinterpret_cast<const load_command*>(p);
    if (c->cmdsize < sizeof(load_command) || p + c->cmdsize > end) break;
    if (c->cmd == LC_UUID) {
      if (wantUuid) uuidOk = (memcmp(reinterpret_cast<const uuid_command*>(p)->uuid, wantUuid, 16) == 0);
    } else if (c->cmd == LC_SYMTAB) {
      auto* st = reinterpret_cast<const symtab_command*>(p);
      symoff = st->symoff; nsyms = st->nsyms; stroff = st->stroff; strsize = st->strsize; haveSym = true;
    }
    p += c->cmdsize;
  }
  if (!uuidOk || !haveSym || nsyms == 0) return false;

  std::vector<struct nlist_64> nl(nsyms);
  std::vector<char> str(strsize ? strsize : 1, '\0');
  if (fseeko(f, (off_t)(off + symoff), SEEK_SET) != 0 ||
      fread(nl.data(), sizeof(struct nlist_64), nsyms, f) != nsyms) return false;
  if (strsize && (fseeko(f, (off_t)(off + stroff), SEEK_SET) != 0 ||
                  fread(str.data(), 1, strsize, f) != strsize)) return false;
  out.reserve(nsyms);
  for (auto& n : nl) {
    const char* nm = "";
    uint32_t sx = n.n_un.n_strx;
    if (sx && sx < strsize) nm = &str[sx];
    out.push_back({n.n_type, n.n_sect, n.n_desc, n.n_value, std::string(nm)});
  }
  return true;
}

// path から LC_SYMTAB を読む（thin / fat 両対応）。wantUuid 指定時はスライスを UUID で選ぶ。
bool loadSymtab(const std::string& path, const uint8_t* uuid, std::vector<NList>& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = false;
  uint32_t magic = 0;
  if (fread(&magic, 4, 1, f) == 1) {
    if (magic == MH_MAGIC_64) {
      ok = readSymtabSlice(f, 0, uuid, out);
    } else if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
      bool swap = (magic == FAT_CIGAM);
      uint32_t nfat = 0;
      if (fread(&nfat, 4, 1, f) == 1) {
        if (swap) nfat = __builtin_bswap32(nfat);
        for (uint32_t i = 0; i < nfat && i < 32; i++) {
          struct fat_arch fa;
          if (fread(&fa, sizeof(fa), 1, f) != 1) break;
          uint64_t sliceOff = swap ? __builtin_bswap32(fa.offset) : fa.offset;
          long save = ftell(f);
          if (readSymtabSlice(f, sliceOff, uuid, out)) { ok = true; break; }
          fseek(f, save, SEEK_SET);
        }
      }
    }
  }
  fclose(f);
  return ok;
}

// ---- .debug_abbrev: code -> 属性(attr,form,implicit) 列 ----
struct AbbrevAttr { uint64_t attr, form; int64_t implicit; };
struct Abbrev { uint64_t tag; bool children; std::vector<AbbrevAttr> attrs; };

std::unordered_map<uint64_t, Abbrev> parseAbbrev(const std::vector<uint8_t>& ab, uint64_t offset) {
  std::unordered_map<uint64_t, Abbrev> m;
  if (offset >= ab.size()) return m;
  Rd r{ab.data() + offset, ab.data() + ab.size()};
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

// form 値を読み飛ばしつつ、文字列系/sec_offset は値を取り出す。
// kindOut: 1=文字列（sOut）, 2=数値（nOut）, 0=その他。addrSize はヘッダ由来。
void readForm(Rd& r, uint64_t form, int addrSize, const std::vector<uint8_t>& str,
              const std::vector<uint8_t>& lineStr, int& kindOut, std::string& sOut, uint64_t& nOut) {
  kindOut = 0;
  switch (form) {
    case 0x01: r.skip(addrSize); break;                 // addr
    case 0x08: sOut = r.cstr(); kindOut = 1; break;     // string(inline)
    case 0x0e: { uint32_t o = r.u32(); if (o < str.size()) { sOut = (const char*)str.data() + o; kindOut = 1; } break; } // strp
    case 0x1f: { uint32_t o = r.u32(); if (o < lineStr.size()) { sOut = (const char*)lineStr.data() + o; kindOut = 1; } break; } // line_strp（DWARF5）
    case 0x0b: case 0x0c: r.skip(1); break;             // data1/flag
    case 0x05: r.skip(2); break;                        // data2
    case 0x06: nOut = r.u32(); kindOut = 2; break;      // data4
    case 0x07: r.skip(8); break;                        // data8
    case 0x1e: r.skip(16); break;                       // data16
    case 0x0d: r.sleb(); break;                         // sdata
    case 0x0f: r.uleb(); break;                         // udata
    case 0x17: nOut = r.u32(); kindOut = 2; break;      // sec_offset(DWARF32)
    case 0x19: break;                                   // flag_present
    case 0x11: r.skip(1); break;                        // ref1
    case 0x12: r.skip(2); break;                        // ref2
    case 0x13: r.skip(4); break;                        // ref4
    case 0x14: r.skip(8); break;                        // ref8
    case 0x15: r.uleb(); break;                         // ref_udata
    case 0x10: r.skip(4); break;                        // ref_addr(DWARF32)
    case 0x18: { uint64_t n = r.uleb(); r.skip(n); break; } // exprloc
    case 0x09: { uint64_t n = r.uleb(); r.skip(n); break; } // block
    case 0x0a: { uint64_t n = r.u8(); r.skip(n); break; }   // block1
    case 0x03: { uint64_t n = r.u16(); r.skip(n); break; }  // block2
    case 0x04: { uint64_t n = r.u32(); r.skip(n); break; }  // block4
    case 0x20: r.skip(8); break;                        // ref_sig8
    case 0x1a: r.uleb(); break;                         // strx（未対応・飛ばす）
    case 0x1b: r.uleb(); break;                         // addrx
    case 0x25: r.skip(1); break;                        // strx1
    case 0x26: r.skip(2); break;                        // strx2
    case 0x28: r.skip(4); break;                        // strx4
    case 0x21: break;                                   // implicit_const（値は abbrev 側）
    default: r.ok = false; break;                       // 未知フォーム → 中断
  }
}

// DWARF5 の行テーブル（ディレクトリ/ファイル）1 エントリ内の 1 フォーム値を読む。
// path は文字列（kind=1, sOut）、directory_index 等は数値（kind=2, nOut）で返す。
// 数値系（data1/2/8, udata）は readForm では値を取り出さない（スキップ）ので、ここで明示的に拾う。
// 文字列系（string/strp/line_strp）とその他は readForm に委譲する。
void readLineForm(Rd& r, uint64_t form, int addrSize, const std::vector<uint8_t>& str,
                  const std::vector<uint8_t>& lineStr, int& kind, std::string& sOut, uint64_t& nOut) {
  kind = 0;
  switch (form) {
    case 0x0b: nOut = r.u8(); kind = 2; break;          // data1
    case 0x05: nOut = r.u16(); kind = 2; break;         // data2
    case 0x07: nOut = r.u64(); kind = 2; break;         // data8
    case 0x0f: nOut = r.uleb(); kind = 2; break;        // udata
    default: readForm(r, form, addrSize, str, lineStr, kind, sOut, nOut); break; // data4/16, strp, line_strp, string...
  }
}

} // namespace

// CU 単位の comp_dir と主ソース名（DW_AT_comp_dir / DW_AT_name）。
struct CuInfo { std::string compDir, name; };

// stmt_list(=.debug_line 内オフセット) → {comp_dir, name} を .debug_info から集める。
// name/comp_dir は、Apple dsymutil が DWARF5 で __debug_line_str を落として行テーブルの
// ファイル名が引けないとき（line_strp 参照が宙に浮く）の表示フォールバックに使う。
static std::unordered_map<uint64_t, CuInfo>
parseCompDirs(const DwarfSections& s, CuInfo& defaultCu) {
  std::unordered_map<uint64_t, CuInfo> out;
  if (s.info.empty() || s.abbrev.empty()) return out;
  Rd r{s.info.data(), s.info.data() + s.info.size()};
  while (r.ok && r.p < r.end) {
    uint32_t len = r.u32();
    if (len == 0xffffffffu || len == 0) break; // DWARF64/不正は非対応
    const uint8_t* cuEnd = r.p + len;
    if (cuEnd > r.end) break;
    uint16_t ver = r.u16();
    uint64_t abbrevOff; int addrSize;
    if (ver >= 5) { r.u8(); addrSize = r.u8(); abbrevOff = r.u32(); } // unit_type, addr, abbrev
    else { abbrevOff = r.u32(); addrSize = r.u8(); }
    auto ab = parseAbbrev(s.abbrev, abbrevOff);
    uint64_t code = r.uleb();
    auto it = ab.find(code);
    std::string compDir, name; uint64_t stmt = 0; bool haveStmt = false;
    if (it != ab.end()) {
      for (auto& a : it->second.attrs) {
        int kind; std::string sv; uint64_t nv = 0;
        readForm(r, a.form, addrSize, s.str, s.lineStr, kind, sv, nv);
        if (a.attr == 0x1b && kind == 1) compDir = sv;        // DW_AT_comp_dir
        else if (a.attr == 0x03 && kind == 1) name = sv;      // DW_AT_name
        else if (a.attr == 0x10 && kind == 2) { stmt = nv; haveStmt = true; } // DW_AT_stmt_list
        if (!r.ok) break;
      }
    }
    if (haveStmt) out[stmt] = CuInfo{compDir, name};
    if (defaultCu.compDir.empty()) defaultCu.compDir = compDir;
    if (defaultCu.name.empty()) defaultCu.name = name;
    r.p = cuEnd; // 次の CU へ（最初の DIE だけ見れば十分）
  }
  return out;
}

// s の .debug_line を解釈し、xlate で変換したアドレスで rows_ へ追加する。
void DwarfLines::parseLineSections(const DwarfSections& s,
                                   const std::function<bool(uint64_t, bool, uint64_t&)>& xlate) {
  CuInfo defCu;
  auto compDirs = parseCompDirs(s, defCu);

  // 表示名+絶対パスを intern する（複数 .o 間でも共有）。
  auto internFile = [this](const std::string& label, const std::string& path) -> int {
    std::string key = label + "\x01" + path;
    auto it = intern_.find(key);
    if (it != intern_.end()) return it->second;
    int idx = (int)fileLabels_.size();
    fileLabels_.push_back(label);
    filePaths_.push_back(path);
    intern_[key] = idx;
    return idx;
  };

  // .debug_line を順に処理
  Rd r{s.line.data(), s.line.data() + s.line.size()};
  while (r.ok && r.p < r.end) {
    uint64_t progOff = (uint64_t)(r.p - s.line.data());
    uint32_t unitLen = r.u32();
    if (unitLen == 0xffffffffu || unitLen == 0) break;
    const uint8_t* progEnd = r.p + unitLen;
    if (progEnd > r.end) break;
    uint16_t ver = r.u16();
    if (ver < 2 || ver > 5) { r.p = progEnd; continue; } // v2〜v5 に対応（それ以外は行表示なしに劣化）
    int addrSize = 8;
    if (ver >= 5) {                              // DWARF5 は header_length の前に 2 バイト増える
      addrSize = r.u8();                         // address_size
      r.u8();                                    // segment_selector_size
    }
    uint32_t headerLen = r.u32();
    const uint8_t* progStart = r.p + headerLen; // オペコード開始（header_length の直後起点）
    uint8_t minInst = r.u8();
    if (ver >= 4) r.u8();                        // max_ops_per_inst（=1 前提）
    uint8_t defaultIsStmt = r.u8(); (void)defaultIsStmt;
    int8_t lineBase = (int8_t)r.u8();
    uint8_t lineRange = r.u8();
    uint8_t opcodeBase = r.u8();
    std::vector<uint8_t> stdLens(opcodeBase > 0 ? opcodeBase - 1 : 0);
    for (size_t i = 0; i < stdLens.size(); i++) stdLens[i] = r.u8();

    std::string compDir = defCu.compDir, cuName = defCu.name;
    { auto it = compDirs.find(progOff);
      if (it != compDirs.end()) {
        if (!it->second.compDir.empty()) compDir = it->second.compDir;
        if (!it->second.name.empty()) cuName = it->second.name;
      } }

    // ディレクトリ名からファイルの絶対パスを組む（絶対はそのまま、相対は comp_dir 前置）。
    auto joinPath = [&](const std::string& fn, std::string dir) -> std::string {
      if (!fn.empty() && fn[0] == '/') return fn;
      if (!dir.empty() && dir[0] != '/' && !compDir.empty()) dir = compDir + "/" + dir;
      if (dir.empty()) dir = compDir;
      return dir.empty() ? fn : dir + "/" + fn;
    };
    // 表示用ラベルは basename（src: <file>:<line> 表示を v2〜v5 で揃える）。
    auto baseName = [](const std::string& p) -> std::string {
      size_t sl = p.find_last_of('/'); return sl == std::string::npos ? p : p.substr(sl + 1);
    };

    std::vector<int> fileGlobal;
    if (ver <= 4) {
      // --- v2〜v4: NUL 終端のディレクトリ列（1 始まり）→ ファイル列（1 始まり）---
      std::vector<std::string> dirs;         // 1 始まり参照、0=comp_dir
      for (;;) { std::string d = r.cstr(); if (d.empty() || !r.ok) break; dirs.push_back(d); }
      fileGlobal.push_back(-1);              // index 0 は未使用（ファイルは 1 始まり）
      for (;;) {
        std::string fn = r.cstr();
        if (fn.empty() || !r.ok) break;
        uint64_t di = r.uleb(); r.uleb(); r.uleb(); // dir_index, mtime, size
        std::string dir = (di == 0) ? compDir : (di <= dirs.size() ? dirs[di - 1] : std::string());
        fileGlobal.push_back(internFile(baseName(fn), joinPath(fn, dir)));
      }
    } else {
      // --- v5: フォーマット記述子つきテーブル（ディレクトリ/ファイルとも 0 始まり）---
      // ディレクトリ[0] は comp_dir、ファイル[0] は主ソース。パスは通常 line_strp。
      auto readTable = [&](std::vector<std::pair<uint64_t, uint64_t>>& fmt, uint64_t& count) {
        uint8_t fc = r.u8();
        fmt.resize(fc);
        for (uint8_t i = 0; i < fc; i++) { fmt[i].first = r.uleb(); fmt[i].second = r.uleb(); } // (content_type, form)
        count = r.uleb();
      };
      std::vector<std::string> dirs;         // 0 始まり
      {
        std::vector<std::pair<uint64_t, uint64_t>> fmt; uint64_t count = 0;
        readTable(fmt, count);
        for (uint64_t i = 0; i < count && r.ok; i++) {
          std::string pathv;
          for (auto& pf : fmt) {
            int kind; std::string sv; uint64_t nv = 0;
            readLineForm(r, pf.second, addrSize, s.str, s.lineStr, kind, sv, nv);
            if (pf.first == 0x1 /*DW_LNCT_path*/ && kind == 1) pathv = sv;
          }
          dirs.push_back(pathv);
        }
      }
      {
        std::vector<std::pair<uint64_t, uint64_t>> fmt; uint64_t count = 0;
        readTable(fmt, count);
        for (uint64_t i = 0; i < count && r.ok; i++) {
          std::string fn; uint64_t di = 0;
          for (auto& pf : fmt) {
            int kind; std::string sv; uint64_t nv = 0;
            readLineForm(r, pf.second, addrSize, s.str, s.lineStr, kind, sv, nv);
            if (pf.first == 0x1 && kind == 1) fn = sv;                 // DW_LNCT_path
            else if (pf.first == 0x2 && kind == 2) di = nv;           // DW_LNCT_directory_index
          }
          // Apple dsymutil は DWARF5 で __debug_line_str を dSYM に残さないことがあり、その場合
          // line_strp 参照（path/dir）が空になる。CU の DW_AT_name / DW_AT_comp_dir で補う。
          if (fn.empty()) fn = cuName;
          std::string dir = (di < dirs.size()) ? dirs[di] : std::string();
          fileGlobal.push_back(internFile(baseName(fn), joinPath(fn, dir)));
        }
      }
    }

    // ---- 行番号プログラム実行 ----
    // file レジスタの初期値は全バージョン 1（DWARF5 でも規格上 1）。v5 は fileGlobal が 0 始まり、
    // v2〜v4 は index 0 が番兵 -1 なので、下限を 0 にしても両対応できる。
    Rd pr{progStart, progEnd};
    uint64_t addr = 0; int file = 1, line = 1; bool endSeq = false;
    auto emit = [&]() {
      int gidx = (file >= 0 && file < (int)fileGlobal.size()) ? fileGlobal[file] : -1;
      uint64_t outAddr = addr;
      if (!xlate(addr, endSeq, outAddr)) return; // 変換不可（マップ外の関数）→ 捨てる
      rows_.push_back({outAddr, gidx, line, endSeq});
    };
    while (pr.ok && pr.p < pr.end) {
      uint8_t op = pr.u8();
      if (op == 0) { // 拡張オペコード
        uint64_t elen = pr.uleb();
        const uint8_t* next = pr.p + elen;
        uint8_t sub = pr.u8();
        if (sub == 1) { endSeq = true; emit(); addr = 0; file = 1; line = 1; endSeq = false; } // end_sequence
        else if (sub == 2) { addr = pr.u64(); }   // set_address（8 byte 前提）
        pr.p = next; // 残りは読み飛ばし
      } else if (op >= opcodeBase) { // 特殊オペコード
        int adj = op - opcodeBase;
        addr += (uint64_t)(adj / lineRange) * minInst;
        line += lineBase + (adj % lineRange);
        emit();
      } else {
        switch (op) {
          case 1: emit(); break;                                  // copy
          case 2: addr += pr.uleb() * minInst; break;             // advance_pc
          case 3: line += (int)pr.sleb(); break;                  // advance_line
          case 4: file = (int)pr.uleb(); break;                   // set_file
          case 5: pr.uleb(); break;                               // set_column
          case 6: break;                                          // negate_stmt
          case 7: break;                                          // set_basic_block
          case 8: { int adj = 255 - opcodeBase; addr += (uint64_t)(adj / lineRange) * minInst; break; } // const_add_pc
          case 9: addr += pr.u16(); break;                        // fixed_advance_pc
          case 10: case 11: break;                                // set_prologue_end / epilogue_begin
          case 12: pr.uleb(); break;                              // set_isa
          default: { int n = (op - 1 < (int)stdLens.size()) ? stdLens[op - 1] : 0; for (int i = 0; i < n; i++) pr.uleb(); break; }
        }
      }
    }
    r.p = progEnd;
  }
}

// dSYM が無いとき、imagePath 本体の debug map（STABS）を辿って各 .o の DWARF を取り込む。
bool DwarfLines::loadViaDebugMap(const std::string& imagePath, const uint8_t uuid[16]) {
  std::vector<NList> syms;
  if (!loadSymtab(imagePath, uuid, syms)) return false;

  // STABS を走査して .o（N_OSO）ごとに関数（N_FUN）を集める。
  //   N_OSO: .o パス（n_value=mtime）
  //   N_FUN: 名前つき = 関数開始（n_value=リンク後アドレス）、名前なし = 直前関数のサイズ（n_value）
  struct Func { std::string name; uint64_t bin; uint64_t size; };
  struct Group { std::string oso; uint64_t mtime; std::vector<Func> funcs; };
  std::vector<Group> groups;
  Group* cur = nullptr;
  Func pend; bool pendOpen = false;
  for (auto& n : syms) {
    if (!(n.type & N_STAB)) continue;
    if (n.type == N_OSO) {
      groups.push_back({n.name, n.value, {}});
      cur = &groups.back();
      pendOpen = false;
    } else if (n.type == N_FUN) {
      if (!n.name.empty()) { pend = {n.name, n.value, 0}; pendOpen = true; }
      else if (pendOpen && cur) { pend.size = n.value; cur->funcs.push_back(pend); pendOpen = false; }
    }
  }
  if (groups.empty()) return false; // debug map が無い（strip 済み/-g 無し）

  bool any = false;
  for (auto& g : groups) {
    // .o の mtime が debug map と食い違う（作り直した）なら行情報がズレるので使わない。
    struct stat stb;
    if (stat(g.oso.c_str(), &stb) != 0) continue;             // .o が見つからない
    if (g.mtime != 0 && (uint64_t)stb.st_mtime != g.mtime) continue; // 古い .o（stale）

    DwarfSections s;
    if (!loadDwarfSections(g.oso, nullptr, s)) continue;      // .o に DWARF が無い

    // .o のシンボル（objAddr 空間）: 関数名 → .o 内アドレス。
    std::vector<NList> osyms;
    if (!loadSymtab(g.oso, nullptr, osyms)) continue;
    std::unordered_map<std::string, uint64_t> objAddr;
    for (auto& n : osyms) {
      if (n.type & N_STAB) continue;
      if ((n.type & N_TYPE) != N_SECT) continue;              // __text 等の実体を持つシンボルだけ
      if (n.name.empty()) continue;
      objAddr.emplace(n.name, n.value);                       // 先勝ち（重複は無視）
    }

    // 関数ごとの区間 [objBeg, objEnd) と差分 delta=bin-obj を作る。
    struct Iv { uint64_t beg, end, delta; };
    std::vector<Iv> ivs;
    for (auto& fn : g.funcs) {
      auto it = objAddr.find(fn.name);
      if (it == objAddr.end()) continue;                      // .o 側に実体が無い（まれ）
      uint64_t ob = it->second;
      uint64_t sz = fn.size ? fn.size : 1;
      ivs.push_back({ob, ob + sz, fn.bin - ob});
    }
    if (ivs.empty()) continue;
    std::sort(ivs.begin(), ivs.end(), [](const Iv& a, const Iv& b) { return a.beg < b.beg; });

    // objAddr 空間の行アドレスを、属する関数の区間 delta でリンク後(unslid)アドレスへ変換。
    // end_sequence 行は関数末尾ちょうど（beg+size）に載るので、所属判定は raw-1 で行う
    // （区間 [beg,end) に収め、変換自体は raw+delta でそのまま末尾を指す）。
    auto xlate = [&ivs](uint64_t raw, bool isEnd, uint64_t& out) -> bool {
      uint64_t probe = (isEnd && raw > 0) ? raw - 1 : raw;
      auto it = std::upper_bound(ivs.begin(), ivs.end(), probe,
                                 [](uint64_t v, const Iv& i) { return v < i.beg; });
      if (it == ivs.begin()) return false;
      --it;
      if (probe >= it->end) return false;                     // 関数の外（パディング/除去済み）
      out = raw + it->delta;
      return true;
    };
    parseLineSections(s, xlate);
    any = true;
  }
  return any;
}

bool DwarfLines::loadForImage(const std::string& imagePath, const uint8_t uuid[16], uint64_t slide) {
  if (imagePath.empty()) return false;
  slide_ = slide;
  size_t sl = imagePath.find_last_of('/');
  std::string name = (sl == std::string::npos) ? imagePath : imagePath.substr(sl + 1);
  std::string dsym = imagePath + ".dSYM/Contents/Resources/DWARF/" + name;

  DwarfSections s;
  if (loadDwarfSections(dsym, uuid, s)) {
    // dSYM: アドレスはリンク済み（unslid）なので恒等変換でそのまま取り込む。
    parseLineSections(s, [](uint64_t a, bool, uint64_t& o) { o = a; return true; });
  } else {
    // dSYM 無し: -g ビルド本体の debug map から各 .o の DWARF を取り込む。
    loadViaDebugMap(imagePath, uuid);
  }

  std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) { return a.addr < b.addr; });
  loaded_ = !rows_.empty();
  return loaded_;
}

bool DwarfLines::lookup(uint64_t runtimeAddr, SourceLoc& out) const {
  if (!loaded_) return false;
  uint64_t a = runtimeAddr - slide_; // unslid（リンク）アドレスで照合
  // a 以下で最大の行。ただし end_sequence 行はギャップ。
  auto it = std::upper_bound(rows_.begin(), rows_.end(), a,
                             [](uint64_t v, const Row& r) { return v < r.addr; });
  if (it == rows_.begin()) return false;
  --it;
  if (it->endSeq) return false;      // シーケンス外（アドレスギャップ）
  if (it->line <= 0) return false;
  if (it->fileIdx < 0 || it->fileIdx >= (int)fileLabels_.size()) return false;
  out.file = fileLabels_[it->fileIdx];
  out.path = filePaths_[it->fileIdx];
  out.line = it->line;
  return true;
}

const std::string& DwarfLines::sourceText(const std::string& path, int line) {
  if (path.empty() || line <= 0) return empty_;
  auto it = srcCache_.find(path);
  if (it == srcCache_.end()) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    if (f) { std::string ln; while (std::getline(f, ln)) lines.push_back(ln); }
    it = srcCache_.emplace(path, std::move(lines)).first;
  }
  const auto& v = it->second;
  if (line - 1 < (int)v.size()) return v[line - 1];
  return empty_;
}

} // namespace ta2
