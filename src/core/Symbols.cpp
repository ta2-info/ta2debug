#include "core/Symbols.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>

#include <sys/stat.h>

#include <mach-o/fat.h>
#include <mach-o/nlist.h>
#include <mach-o/loader.h>

namespace ta2 {

std::string Symbols::keyFor(const Image& img) {
  if (img.hasUuid) {
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02X", img.uuid[i]);
    return std::string(hex, 32);
  }
  return img.path + "@" + std::to_string(img.loadAddr);
}

// ~/Library/Caches/ta2debug/syms （無ければ親ごと作成）。HOME 不明なら空。
std::string Symbols::cacheDir() {
  const char* home = getenv("HOME");
  if (!home || !*home) return "";
  std::string base = std::string(home) + "/Library/Caches/ta2debug";
  mkdir(base.c_str(), 0755);            // 既存なら EEXIST（無視）
  std::string dir = base + "/syms";
  mkdir(dir.c_str(), 0755);
  return dir;
}

std::string Symbols::cachePath(const Image& img) {
  if (!img.hasUuid) return "";
  std::string dir = cacheDir();
  if (dir.empty()) return "";
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", img.uuid[i]);
  return dir + "/" + std::string(hex, 32) + ".t2s"; // UUID 別名＝更新で自動的に別ファイル
}

namespace { constexpr uint32_t kMagic = 0x54325332; } // 'T2S2'（dSYM 対応で版上げ＝旧キャッシュは作り直し）
std::string demangleName(const std::string& mangled); // 前方宣言（下で定義）

// UUID キャッシュから読む。形式: magic, count, {unslid(u64), nameLen(u16), name} ×count。
bool Symbols::loadFromDisk(const Image& img, std::vector<Symbol>& out) {
  std::string path = cachePath(img);
  if (path.empty()) return false;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = false;
  uint32_t magic = 0, count = 0;
  if (fread(&magic, 4, 1, f) == 1 && magic == kMagic && fread(&count, 4, 1, f) == 1 && count < 5000000) {
    out.reserve(count); ok = true;
    for (uint32_t i = 0; i < count; i++) {
      uint64_t unslid = 0; uint16_t len = 0;
      if (fread(&unslid, 8, 1, f) != 1 || fread(&len, 2, 1, f) != 1) { ok = false; break; }
      std::string name(len, '\0');
      if (len && fread(&name[0], 1, len, f) != len) { ok = false; break; }
      Symbol s; s.addr = unslid + img.slide; s.mangled = name; // demangled は遅延
      out.push_back(std::move(s));
    }
  }
  fclose(f);
  if (!ok) out.clear();
  return ok;
}

// UUID キャッシュへ保存（slide を抜いた unslid 値で）。temp→rename で原子的に。
void Symbols::saveToDisk(const Image& img, const std::vector<Symbol>& syms) {
  std::string path = cachePath(img);
  if (path.empty() || syms.empty()) return;
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  uint32_t count = (uint32_t)syms.size();
  bool ok = fwrite(&kMagic, 4, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1;
  for (const auto& s : syms) {
    if (!ok) break;
    uint64_t unslid = s.addr - img.slide;
    uint16_t len = (uint16_t)std::min<size_t>(s.mangled.size(), 65535);
    ok = fwrite(&unslid, 8, 1, f) == 1 && fwrite(&len, 2, 1, f) == 1 &&
         (len == 0 || fwrite(s.mangled.data(), 1, len, f) == len);
  }
  fclose(f);
  if (ok) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str());
}

static void replaceAll(std::string& s, const std::string& a, const std::string& b) {
  size_t p = 0;
  while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
}

// 長いテンプレート引数群を "<…>" に省略する（外側優先・ネスト対応のバランス走査）。
// 閾値以下の短い引数（<int> など）はそのまま残す。"operator<<" / "<<" は誤爆しないよう素通り。
static std::string collapseTemplates(const std::string& s, size_t thresh) {
  std::string out; out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '<') {
      bool isOp = (i >= 8 && s.compare(i - 8, 8, "operator") == 0) || (i > 0 && s[i - 1] == '<');
      if (isOp) { out += s[i++]; continue; } // 演算子名の '<' はテンプレートではない
      int depth = 0; size_t j = i;
      for (; j < s.size(); j++) { if (s[j] == '<') depth++; else if (s[j] == '>') { depth--; if (!depth) break; } }
      if (j < s.size()) { // i..j が対応する <...>
        size_t inner = j - i - 1;
        if (inner > thresh) out += "<…>";                                   // 長い → 省略
        else { out += '<'; out += collapseTemplates(s.substr(i + 1, inner), thresh); out += '>'; } // 短い → 残す
        i = j + 1; continue;
      }
    }
    out += s[i++];
  }
  return out;
}

// デマングル後の表示用整形: libc++ の内部 inline namespace を畳み、長いテンプレートを省略。
static std::string abbreviate(const std::string& in) {
  std::string s = in;
  replaceAll(s, "std::__1::", "std::");
  replaceAll(s, "std::__cxx11::", "std::");
  return collapseTemplates(s, 24);
}

std::string demangleName(const std::string& mangled) {
  if (mangled.size() > 2 && mangled[0] == '_' && mangled[1] == 'Z') {
    int st = 0;
    char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &st);
    if (st == 0 && d) { std::string r(d); free(d); return abbreviate(r); }
  }
  return mangled;
}

// nlist 1 本を Symbol に変換して out へ（共通処理）。name は文字列テーブル内の先頭。
static void addNlistSymbol(std::vector<Symbol>& out, const struct nlist_64& n, const char* name, uint64_t slide) {
  if (n.n_type & N_STAB) return;      // デバッグ用スタブは除く
  if (n.n_value == 0) return;
  if (!name || *name == '\0') return;
  Symbol sym;
  sym.addr = n.n_value + slide;
  std::string raw(name);
  // macOS Mach-O は C/C++ 名に先頭 '_' が付く。1 つ落とした形が Itanium の mangled 名。
  sym.mangled = (raw[0] == '_') ? raw.substr(1) : raw;
  // デマングルは遅延（displayName）。全件デマングルは巨大バイナリで非常に重いため行わない。
  out.push_back(std::move(sym));
}

void Symbols::load(const Image& img, std::vector<Symbol>& out) {
  if (loadFromDisk(img, out)) return; // UUID 一致のディスクキャッシュがあれば読み直さない

  // (1) 対象メモリ内の symtab（あれば）。stripped でも下の dSYM で補える。
  const Segment* linkedit = nullptr;
  for (const auto& s : img.segments) if (s.name == "__LINKEDIT") { linkedit = &s; break; }
  if (img.hasSymtab && img.nsyms > 0 && linkedit) {
    auto fileToRuntime = [&](uint64_t fo) { return linkedit->vmaddr + img.slide + (fo - linkedit->fileoff); };
    std::vector<struct nlist_64> nl(img.nsyms);
    if (target_.readMemory(fileToRuntime(img.symoff), nl.data(), nl.size() * sizeof(struct nlist_64))) {
      // 文字列テーブルは「このイメージの採用シンボルが参照する部分」だけを、まとまり（チャンク）
      // 単位のまとめ読みで取る。dyld 共有キャッシュ内のライブラリは strsize が共有キャッシュ全体の
      // 巨大な文字列プール（約 100MB）を指し、しかも文字列は全ライブラリで重複排除されているため
      // 各ライブラリの参照先がプール全域に散らばる。全体（や min〜max）を読むと 1 イメージごとに
      // 巨大バッファの確保・ゼロ埋め・解放だけで約 2 秒かかり、全イメージ横断探索が cold で
      // 1 分以上になっていた。採用シンボルの添字を昇順に並べ、間隔の大きい所で最大 kMaxChunks 個に
      // 分割して各チャンクを 1 回ずつ読む（1 本ずつは読まない）。通常のバイナリは添字が密なので
      // 分割されず従来どおりテーブル全体の 1 回読みになる（挙動は不変）。
      std::vector<uint32_t> xs; xs.reserve(nl.size());
      for (auto& n : nl) {
        uint32_t sx = n.n_un.n_strx;
        if (sx == 0 || sx >= img.strsize) continue;             // 従来どおり無効添字は飛ばす
        if ((n.n_type & N_STAB) || n.n_value == 0) continue;    // addNlistSymbol が捨てる物は読まない
        xs.push_back(sx);
      }
      std::sort(xs.begin(), xs.end());
      xs.erase(std::unique(xs.begin(), xs.end()), xs.end());

      const size_t   kMaxChunks  = 256;        // 1 イメージあたりの読み回数上限
      const uint64_t kMinGap     = 4096;       // これ以下の隙間は分割しない（読み回数を増やさない）
      const uint64_t kTailMargin = 16 * 1024;  // チャンク末尾の文字列の終端 NUL を含めるための先読み幅

      struct StrChunk { uint64_t beg = 0, end = 0; std::vector<char> buf; }; // [beg,end) の文字列バイト
      std::vector<StrChunk> chunks;
      if (!xs.empty()) {
        // 分割位置: 隣接添字の間隔が kMinGap より大きい所のうち、大きい順に kMaxChunks-1 個まで。
        std::vector<std::pair<uint64_t, size_t>> gaps; // (間隔, xs の添字 i: xs[i-1] と xs[i] の間)
        for (size_t i = 1; i < xs.size(); i++) {
          uint64_t g = (uint64_t)xs[i] - xs[i - 1];
          if (g > kMinGap) gaps.push_back({g, i});
        }
        std::sort(gaps.begin(), gaps.end(), [](const std::pair<uint64_t, size_t>& a, const std::pair<uint64_t, size_t>& b) { return a.first > b.first; });
        if (gaps.size() > kMaxChunks - 1) gaps.resize(kMaxChunks - 1);
        std::vector<size_t> splits;
        for (auto& g : gaps) splits.push_back(g.second);
        std::sort(splits.begin(), splits.end());
        splits.push_back(xs.size()); // 番兵（最後のチャンクの終わり）

        size_t from = 0;
        for (size_t sp : splits) {
          uint64_t first = xs[from], last = xs[sp - 1];
          from = sp;
          StrChunk c; c.beg = first;
          // 末尾の文字列は長さが分からないので先読み幅ぶん余分に読む（テーブル末尾で打ち切り）。
          // その中に終端 NUL が無ければ幅を広げて読み直す（まれな超長名。テーブル末尾まで広げれば必ず止まる）。
          for (uint64_t margin = kTailMargin;; margin *= 4) {
            c.end = std::min<uint64_t>(last + margin, img.strsize);
            c.buf.assign((size_t)(c.end - c.beg) + 1, '\0'); // +1: 末尾を必ず NUL にして読み過ぎを防ぐ
            if (!target_.readMemory(fileToRuntime((uint64_t)img.stroff + c.beg), c.buf.data(), (size_t)(c.end - c.beg))) {
              c.buf.clear(); c.end = c.beg; break;           // 読めないチャンクは捨てる（該当シンボルは入らない）
            }
            if (c.end >= img.strsize || memchr(&c.buf[(size_t)(last - c.beg)], 0, (size_t)(c.end - last))) break;
          }
          chunks.push_back(std::move(c));
        }
      }

      // 添字 sx を含むチャンク内の文字列先頭（無ければ nullptr）。chunks は beg 昇順。
      auto strAt = [&](uint32_t sx) -> const char* {
        auto it = std::upper_bound(chunks.begin(), chunks.end(), (uint64_t)sx,
                                   [](uint64_t v, const StrChunk& c) { return v < c.beg; });
        if (it == chunks.begin()) return nullptr;
        --it;
        if (sx >= it->end) return nullptr;
        return &it->buf[(size_t)(sx - it->beg)];
      };
      out.reserve(out.size() + xs.size());
      for (auto& n : nl) {
        uint32_t sx = n.n_un.n_strx;
        if (sx == 0 || sx >= img.strsize) continue;
        if ((n.n_type & N_STAB) || n.n_value == 0) continue;
        const char* name = strAt(sx);
        if (name) addNlistSymbol(out, n, name, img.slide);
      }
    }
  }

  // (2) 隣接 dSYM の symtab（UUID 一致時のみ）。stripped バイナリでも関数名が得られる。
  loadDsymSymbols(img, out);

  std::sort(out.begin(), out.end(), [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
  out.erase(std::unique(out.begin(), out.end(),
                        [](const Symbol& a, const Symbol& b) { return a.addr == b.addr; }),
            out.end());
  saveToDisk(img, out); // 次回・別セッション用に UUID キャッシュへ保存
}

// ファイル f の offset から Mach-O(64) を読み、LC_UUID が wantUuid と一致すれば LC_SYMTAB を out へ。
// 一致して symtab を読めたら true。
static bool readSymtabFromSlice(FILE* f, uint64_t off, const uint8_t wantUuid[16], uint64_t slide,
                                std::vector<Symbol>& out) {
  struct mach_header_64 mh;
  if (fseeko(f, (off_t)off, SEEK_SET) != 0 || fread(&mh, sizeof(mh), 1, f) != 1) return false;
  if (mh.magic != MH_MAGIC_64) return false; // 64bit リトルエンディアンのみ対応（ホストと同じ）
  std::vector<uint8_t> lc(mh.sizeofcmds);
  if (fread(lc.data(), 1, mh.sizeofcmds, f) != mh.sizeofcmds) return false;

  bool uuidOk = false; uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0; bool haveSym = false;
  const uint8_t* p = lc.data(); const uint8_t* end = p + mh.sizeofcmds;
  for (uint32_t i = 0; i < mh.ncmds && p + sizeof(load_command) <= end; i++) {
    auto* c = reinterpret_cast<const load_command*>(p);
    if (c->cmdsize < sizeof(load_command) || p + c->cmdsize > end) break;
    if (c->cmd == LC_UUID) {
      auto* u = reinterpret_cast<const uuid_command*>(p);
      uuidOk = (memcmp(u->uuid, wantUuid, 16) == 0);
    } else if (c->cmd == LC_SYMTAB) {
      auto* st = reinterpret_cast<const symtab_command*>(p);
      symoff = st->symoff; nsyms = st->nsyms; stroff = st->stroff; strsize = st->strsize; haveSym = true;
    }
    p += c->cmdsize;
  }
  if (!uuidOk || !haveSym || nsyms == 0) return false;

  std::vector<struct nlist_64> nl(nsyms);
  std::vector<char> str(strsize);
  if (fseeko(f, (off_t)(off + symoff), SEEK_SET) != 0 || fread(nl.data(), sizeof(struct nlist_64), nsyms, f) != nsyms) return false;
  if (fseeko(f, (off_t)(off + stroff), SEEK_SET) != 0 || fread(str.data(), 1, strsize, f) != strsize) return false;
  for (auto& n : nl) {
    uint32_t sx = n.n_un.n_strx;
    if (sx == 0 || sx >= strsize) continue;
    addNlistSymbol(out, n, &str[sx], slide);
  }
  return true;
}

void Symbols::loadDsymSymbols(const Image& img, std::vector<Symbol>& out) {
  if (!img.hasUuid || img.path.empty()) return;
  size_t sl = img.path.find_last_of('/');
  std::string name = (sl == std::string::npos) ? img.path : img.path.substr(sl + 1);
  std::string dsym = img.path + ".dSYM/Contents/Resources/DWARF/" + name;
  FILE* f = fopen(dsym.c_str(), "rb");
  if (!f) return;
  uint32_t magic = 0;
  if (fread(&magic, 4, 1, f) == 1) {
    if (magic == MH_MAGIC_64) {                       // thin（64bit LE）
      readSymtabFromSlice(f, 0, img.uuid, img.slide, out);
    } else if (magic == FAT_MAGIC || magic == FAT_CIGAM) { // fat: UUID 一致スライスを探す
      auto be32 = [](uint32_t v) { return __builtin_bswap32(v); };
      bool swap = (magic == FAT_CIGAM);
      uint32_t nfat = 0;
      if (fread(&nfat, 4, 1, f) == 1) {
        if (swap) nfat = be32(nfat);
        for (uint32_t i = 0; i < nfat && i < 32; i++) {
          struct fat_arch fa;
          if (fread(&fa, sizeof(fa), 1, f) != 1) break;
          uint64_t sliceOff = swap ? be32(fa.offset) : fa.offset;
          long save = ftell(f);
          if (readSymtabFromSlice(f, sliceOff, img.uuid, img.slide, out)) break;
          fseek(f, save, SEEK_SET);
        }
      }
    }
  }
  fclose(f);
}

std::string Symbols::stubTarget(const Image& img, uint64_t addr) {
  if (!img.containsStub(addr) || !img.stubEntrySize || !img.hasDysymtab || !img.hasSymtab) return "";

  const Segment* linkedit = nullptr;
  for (const auto& s : img.segments) if (s.name == "__LINKEDIT") { linkedit = &s; break; }
  if (!linkedit) return "";
  auto f2r = [&](uint64_t fo) { return linkedit->vmaddr + img.slide + (fo - linkedit->fileoff); };

  // スタブ番号 → 間接シンボルテーブル添字 → シンボルテーブル添字
  uint32_t slot = (uint32_t)((addr - (img.stubVmaddr + img.slide)) / img.stubEntrySize);
  uint32_t indIdx = img.stubIndirectIndex + slot;
  if (indIdx >= img.nindirectsyms) return "";
  uint32_t symIndex = 0;
  if (!target_.readMemory(f2r(img.indirectsymoff) + (uint64_t)indIdx * 4, &symIndex, 4)) return "";
  if ((symIndex & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) || symIndex >= img.nsyms) return "";

  struct nlist_64 n = {};
  if (!target_.readMemory(f2r(img.symoff) + (uint64_t)symIndex * sizeof(n), &n, sizeof(n))) return "";
  uint32_t sx = n.n_un.n_strx;
  if (sx == 0 || sx >= img.strsize) return "";
  char buf[256] = {};
  if (!target_.readMemory(f2r(img.stroff) + sx, buf, sizeof(buf) - 1)) return "";
  std::string raw(buf);
  if (raw.empty()) return "";
  // 先頭 '_' を落とした Itanium 名をデマングル（通常シンボルと同じ扱い。C 名はそのまま返る）。
  return demangleName(raw[0] == '_' ? raw.substr(1) : raw);
}

const std::vector<Symbol>& Symbols::forImage(const Image& img) {
  std::string key = keyFor(img);
  // まず短時間ロックでキャッシュ確認（既存なら即返す）。重い load は↓でロック外に出す。
  // これで warming が重いライブラリを読んでいる間も、本線（描画の describe/symbolName 等）は
  // 既にキャッシュ済みの別ライブラリへロック競合せずアクセスでき、UI が固まらない。
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
  }
  std::vector<Symbol> v;
  load(img, v); // メモリ読込・パース・ディスク I/O（ロック外）
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cache_.find(key); // 競合で他スレッドが先に入れていれば再利用
  if (it != cache_.end()) return it->second;
  return cache_.emplace(key, std::move(v)).first->second;
}

const Symbol* Symbols::byAddress(const Image& img, uint64_t addr) {
  const auto& v = forImage(img);
  if (v.empty()) return nullptr;
  // addr 以下で最も近い
  auto it = std::upper_bound(v.begin(), v.end(), addr,
                             [](uint64_t a, const Symbol& s) { return a < s.addr; });
  if (it == v.begin()) return nullptr;
  --it;
  return &*it;
}

const std::string& Symbols::displayName(const Symbol& s) {
  // mu_ で保護する。シンボル探索（byName）が別スレッドで走り、メインスレッドの描画
  // （describe/symbolName）と同じ Symbol に同時アクセスし得るため。demangled は一度だけ
  // 空→値に書かれ以後不変なので、ロックは初回デマングルの直列化のためだけに要る。
  std::lock_guard<std::mutex> lk(mu_);
  if (s.demangled.empty() && !s.mangled.empty()) s.demangled = demangleName(s.mangled);
  return s.demangled;
}

// デマングル後の関数名から、引数リストを開く '(' の位置を返す（関数でなければ npos）。
// 末尾の ')' から括弧の対応を取って左へ戻るので、名前の一部である "(anonymous namespace)"
// の先頭 '(' や、ラムダ "operator()" の '()' を引数リストと誤認しない（最初の '(' を
// 使うと匿名名前空間シンボルが空名に潰れて一致しなくなる）。
static size_t argListOpenParen(const std::string& dm) {
  size_t close = dm.rfind(')');
  if (close == std::string::npos) return std::string::npos;  // 関数でない（データ等）
  int depth = 0;
  for (size_t i = close + 1; i > 0; ) {
    char c = dm[--i];
    if (c == ')') depth++;
    else if (c == '(') { if (--depth == 0) return i; }
  }
  return std::string::npos;
}

std::vector<const Symbol*> Symbols::byName(const Image& img, const std::string& name) {
  const auto& v = forImage(img);
  std::vector<const Symbol*> hits;

  // デマングル対象を絞るための "probe"（名前の末尾識別子）。Itanium は識別子を <len>name で
  // 埋め込むので、mangled に probe が部分文字列として含まれない限りデマングル名も一致しない。
  // これで巨大なシンボルテーブルでも全件デマングルを避ける。識別子以外（operator 等）は絞らない。
  std::string probe = name;
  { size_t par = probe.find('('); if (par != std::string::npos) probe = probe.substr(0, par);
    size_t cc = probe.rfind("::"); if (cc != std::string::npos) probe = probe.substr(cc + 2); }
  bool canPrune = !probe.empty();
  for (char c : probe) if (!(std::isalnum((unsigned char)c) || c == '_')) { canPrune = false; break; }
  auto skip = [&](const Symbol& s) { return canPrune && s.mangled.find(probe) == std::string::npos; };

  // 1) mangled 完全一致（C 名など。デマングル不要） / デマングル名完全一致
  for (const auto& s : v) {
    if (s.mangled == name) { hits.push_back(&s); continue; }
    if (skip(s)) continue;
    if (displayName(s) == name) hits.push_back(&s);
  }
  // 2) 引数を除いた名前一致（"demo::run" が "demo::run(int)" に一致）
  if (hits.empty() && name.find('(') == std::string::npos) {
    for (const auto& s : v) {
      if (skip(s)) continue;
      const std::string& dm = displayName(s);
      size_t paren = argListOpenParen(dm);
      if (paren != std::string::npos && paren == name.size() && dm.compare(0, paren, name) == 0)
        hits.push_back(&s);
    }
  }
  // 3) 末尾の識別子一致（修飾なしのバレ名で C++ の名前空間／クラス内関数を引けるように）。
  //    "Decode" が "demo::Player::Decode(int)" に一致する。1・2 で引けなかった時だけのフォールバック。
  //    ユーザが既に修飾／シグネチャを書いた場合（:: や ( を含む）は曖昧一致をしない。
  //    複数ヒットは上位（parseName）が「複数ある」と報告して明示指定を促す。
  if (hits.empty() && name.find("::") == std::string::npos && name.find('(') == std::string::npos) {
    for (const auto& s : v) {
      if (skip(s)) continue;
      const std::string& dm = displayName(s);
      size_t paren = argListOpenParen(dm);                  // 引数リストの '(' 手前までが名前
      std::string base = (paren == std::string::npos) ? dm : dm.substr(0, paren);
      size_t cc = base.rfind("::");                          // 最後の '::' の後ろが末尾識別子
      const std::string& tail = (cc == std::string::npos) ? base : base.substr(cc + 2);
      if (tail == name) hits.push_back(&s);
    }
  }
  return hits;
}

} // namespace ta2
