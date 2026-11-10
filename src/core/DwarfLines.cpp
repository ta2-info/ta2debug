#include "core/DwarfLines.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <mach-o/fat.h>
#include <mach-o/loader.h>

namespace ta2 {
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

// DWARF4 の __debug セクション群（必要なものだけ）。
struct Sections {
  std::vector<uint8_t> line, info, abbrev, str;
};

// Mach-O(64) スライスから __DWARF の各セクションを読む。uuid 一致・debug_line 必須。
bool readDwarfSlice(FILE* f, uint64_t off, const uint8_t wantUuid[16], Sections& out) {
  struct mach_header_64 mh;
  if (fseeko(f, (off_t)off, SEEK_SET) != 0 || fread(&mh, sizeof(mh), 1, f) != 1) return false;
  if (mh.magic != MH_MAGIC_64) return false;
  std::vector<uint8_t> lc(mh.sizeofcmds);
  if (fread(lc.data(), 1, mh.sizeofcmds, f) != mh.sizeofcmds) return false;

  bool uuidOk = false;
  struct Sec { uint64_t fileoff; uint64_t size; std::vector<uint8_t>* dst; };
  std::vector<Sec> want;
  const uint8_t* p = lc.data(); const uint8_t* end = p + mh.sizeofcmds;
  for (uint32_t i = 0; i < mh.ncmds && p + sizeof(load_command) <= end; i++) {
    auto* c = reinterpret_cast<const load_command*>(p);
    if (c->cmdsize < sizeof(load_command) || p + c->cmdsize > end) break;
    if (c->cmd == LC_UUID) {
      uuidOk = (memcmp(reinterpret_cast<const uuid_command*>(p)->uuid, wantUuid, 16) == 0);
    } else if (c->cmd == LC_SEGMENT_64) {
      auto* sg = reinterpret_cast<const segment_command_64*>(p);
      if (strncmp(sg->segname, "__DWARF", 16) == 0) {
        auto* s = reinterpret_cast<const section_64*>(p + sizeof(segment_command_64));
        for (uint32_t j = 0; j < sg->nsects; j++, s++) {
          std::vector<uint8_t>* dst = nullptr;
          if (!strncmp(s->sectname, "__debug_line", 16)) dst = &out.line;
          else if (!strncmp(s->sectname, "__debug_info", 16)) dst = &out.info;
          else if (!strncmp(s->sectname, "__debug_abbrev", 16)) dst = &out.abbrev;
          else if (!strncmp(s->sectname, "__debug_str", 16)) dst = &out.str;
          if (dst) want.push_back({s->offset, s->size, dst});
        }
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

bool loadDwarfSections(const std::string& dsymPath, const uint8_t uuid[16], Sections& out) {
  FILE* f = fopen(dsymPath.c_str(), "rb");
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
              int& kindOut, std::string& sOut, uint64_t& nOut) {
  kindOut = 0;
  switch (form) {
    case 0x01: r.skip(addrSize); break;                 // addr
    case 0x08: sOut = r.cstr(); kindOut = 1; break;     // string(inline)
    case 0x0e: { uint32_t o = r.u32(); if (o < str.size()) { sOut = (const char*)str.data() + o; kindOut = 1; } break; } // strp
    case 0x1f: { r.u32(); break; }                      // line_strp（.debug_line_str 未保持→スキップ）
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

} // namespace

// stmt_list(=.debug_line 内オフセット) → comp_dir を .debug_info から集める。
static std::unordered_map<uint64_t, std::string>
parseCompDirs(const Sections& s, std::string& defaultCompDir) {
  std::unordered_map<uint64_t, std::string> out;
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
        readForm(r, a.form, addrSize, s.str, kind, sv, nv);
        if (a.attr == 0x1b && kind == 1) compDir = sv;        // DW_AT_comp_dir
        else if (a.attr == 0x03 && kind == 1) name = sv;      // DW_AT_name
        else if (a.attr == 0x10 && kind == 2) { stmt = nv; haveStmt = true; } // DW_AT_stmt_list
        if (!r.ok) break;
      }
    }
    if (haveStmt) out[stmt] = compDir;
    if (defaultCompDir.empty()) defaultCompDir = compDir;
    r.p = cuEnd; // 次の CU へ（最初の DIE だけ見れば十分）
  }
  return out;
}

bool DwarfLines::loadForImage(const std::string& imagePath, const uint8_t uuid[16], uint64_t slide) {
  if (imagePath.empty()) return false;
  size_t sl = imagePath.find_last_of('/');
  std::string name = (sl == std::string::npos) ? imagePath : imagePath.substr(sl + 1);
  std::string dsym = imagePath + ".dSYM/Contents/Resources/DWARF/" + name;
  Sections s;
  if (!loadDwarfSections(dsym, uuid, s)) return false;
  slide_ = slide;

  std::string defCompDir;
  auto compDirs = parseCompDirs(s, defCompDir);

  // 表示名+絶対パスを intern する
  std::unordered_map<std::string, int> intern;
  auto internFile = [&](const std::string& label, const std::string& path) -> int {
    std::string key = label + "\x01" + path;
    auto it = intern.find(key);
    if (it != intern.end()) return it->second;
    int idx = (int)fileLabels_.size();
    fileLabels_.push_back(label);
    filePaths_.push_back(path);
    intern[key] = idx;
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
    if (ver < 2 || ver > 4) { r.p = progEnd; continue; } // v5 等は今回は飛ばす（行表示なしに劣化）
    uint32_t headerLen = r.u32();
    const uint8_t* progStart = r.p + headerLen; // オペコード開始
    uint8_t minInst = r.u8();
    if (ver >= 4) r.u8();                        // max_ops_per_inst（=1 前提）
    uint8_t defaultIsStmt = r.u8(); (void)defaultIsStmt;
    int8_t lineBase = (int8_t)r.u8();
    uint8_t lineRange = r.u8();
    uint8_t opcodeBase = r.u8();
    std::vector<uint8_t> stdLens(opcodeBase > 0 ? opcodeBase - 1 : 0);
    for (size_t i = 0; i < stdLens.size(); i++) stdLens[i] = r.u8();

    std::string compDir = defCompDir;
    { auto it = compDirs.find(progOff); if (it != compDirs.end() && !it->second.empty()) compDir = it->second; }

    std::vector<std::string> dirs;         // 1 始まり参照、0=comp_dir
    for (;;) { std::string d = r.cstr(); if (d.empty() || !r.ok) break; dirs.push_back(d); }
    // file_names（1 始まり）
    std::vector<int> fileGlobal; fileGlobal.push_back(-1); // index 0 は未使用
    for (;;) {
      std::string fn = r.cstr();
      if (fn.empty() || !r.ok) break;
      uint64_t di = r.uleb(); r.uleb(); r.uleb(); // dir_index, mtime, size
      std::string label = fn, path;
      auto joinDir = [&](const std::string& base) { if (fn[0] == '/') path = fn; else if (!base.empty()) path = base + "/" + fn; };
      if (fn[0] == '/') path = fn;
      else {
        std::string dir = (di == 0) ? compDir : (di <= dirs.size() ? dirs[di - 1] : std::string());
        if (!dir.empty() && dir[0] != '/' && !compDir.empty()) dir = compDir + "/" + dir;
        joinDir(dir.empty() ? compDir : dir);
      }
      fileGlobal.push_back(internFile(label, path));
    }

    // ---- 行番号プログラム実行 ----
    Rd pr{progStart, progEnd};
    uint64_t addr = 0; int file = 1, line = 1; bool endSeq = false;
    auto emit = [&]() {
      int gidx = (file >= 1 && file < (int)fileGlobal.size()) ? fileGlobal[file] : -1;
      rows_.push_back({addr, gidx, line, endSeq});
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
