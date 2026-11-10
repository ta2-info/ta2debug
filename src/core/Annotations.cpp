#include "core/Annotations.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

namespace ta2 {

// root（sudo）で動いていると作成ファイルが root 所有になり、ユーザが手編集するのに sudo が要る。
// 条件1（普通のエディタで編集できる）を満たすため、sudo 実行時は呼び出しユーザ（SUDO_UID/GID）へ
// 所有権を戻す。best-effort（失敗は無視）。root でない通常実行では何もしない。
static void chownToInvoker(const std::string& path) {
  if (geteuid() != 0) return;
  const char* su = getenv("SUDO_UID");
  const char* sg = getenv("SUDO_GID");
  if (!su || !*su) return;
  uid_t uid = (uid_t)strtoul(su, nullptr, 10);
  gid_t gid = (sg && *sg) ? (gid_t)strtoul(sg, nullptr, 10) : (gid_t)-1;
  if (uid == 0) return;
  (void)chown(path.c_str(), uid, gid);
}

std::string Annotations::uuidHex(const Image& img) {
  if (!img.hasUuid) return "";
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02X", img.uuid[i]);
  return std::string(hex, 32);
}

// ~/Library/Application Support/ta2debug/annotations （親ごと作成）。
// Caches ではなく Application Support＝「消さないユーザデータ」の場所に置く。
std::string Annotations::dir() {
  const char* home = getenv("HOME");
  if (!home || !*home) return "";
  std::string a = std::string(home) + "/Library/Application Support";
  mkdir(a.c_str(), 0755); // 既存なら EEXIST（無視）
  std::string b = a + "/ta2debug";
  mkdir(b.c_str(), 0755);
  std::string c = b + "/annotations";
  mkdir(c.c_str(), 0755);
  chownToInvoker(b); chownToInvoker(c); // sudo 実行時はユーザ所有へ（手編集できるように）
  return c;
}

std::string Annotations::pathFor(const Image& img) {
  std::string u = uuidHex(img);
  if (u.empty()) return "";
  std::string d = dir();
  if (d.empty()) return "";
  return d + "/" + u + ".t2a";
}

// 1 行をパースして Annot を out へ。無効行は false（呼び出し側が無視）。TAB 区切り優先、
// 無ければアドレスと名前を空白で分ける。アドレスは常に 16 進（0x 省略可）。
static bool parseLine(const std::string& raw, Annot& a) {
  std::string line = raw;
  // 末尾の CR/改行を落とす（CRLF 耐性）
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  // 先頭の空白を飛ばす
  size_t i = 0; while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i >= line.size()) return false;    // 空行
  if (line[i] == '#') return false;      // コメント
  line = line.substr(i);

  std::string addrTok, name, memo;
  size_t t1 = line.find('\t');
  if (t1 != std::string::npos) {                 // TAB 区切り: addr \t name \t memo
    addrTok = line.substr(0, t1);
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 != std::string::npos) { name = line.substr(t1 + 1, t2 - t1 - 1); memo = line.substr(t2 + 1); }
    else name = line.substr(t1 + 1);
  } else {                                        // 空白区切り: addr name（メモ無し）
    size_t sp = line.find_first_of(" \t");
    if (sp == std::string::npos) addrTok = line;
    else { addrTok = line.substr(0, sp); size_t ns = line.find_first_not_of(" \t", sp); if (ns != std::string::npos) name = line.substr(ns); }
  }

  // アドレス（16 進。0x 接頭辞は許容）
  std::string h = addrTok;
  if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
  if (h.empty()) return false;
  char* end = nullptr;
  unsigned long long v = strtoull(h.c_str(), &end, 16);
  if (end == h.c_str() || (end && *end != '\0')) return false; // 16 進として読み切れない

  // name / memo の前後空白を軽く整える（memo の末尾空白のみ除去。先頭は残す＝意図的な空白を尊重）
  auto trimBoth = [](std::string s) {
    size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t"); return s.substr(a, b - a + 1);
  };
  while (!memo.empty() && (memo.back() == ' ' || memo.back() == '\t')) memo.pop_back();
  a.unslid = (uint64_t)v;
  a.name = trimBoth(name);
  a.memo = memo;
  return true;
}

bool Annotations::parseFile(const std::string& path, std::vector<Annot>& out) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  char* buf = nullptr; size_t cap = 0; ssize_t n;
  while ((n = getline(&buf, &cap, f)) != -1) {
    Annot a;
    if (parseLine(std::string(buf, (size_t)n), a)) out.push_back(std::move(a));
  }
  free(buf);
  fclose(f);
  // 同一アドレスは後勝ちにまとめる（手編集で重複しても壊れない）
  std::sort(out.begin(), out.end(), [](const Annot& x, const Annot& y) { return x.unslid < y.unslid; });
  std::vector<Annot> dedup;
  for (auto& a : out) {
    if (!dedup.empty() && dedup.back().unslid == a.unslid) dedup.back() = a; // 後勝ち
    else dedup.push_back(a);
  }
  out.swap(dedup);
  return true;
}

void Annotations::rebuildSyms(Entry& e) {
  e.syms.clear();
  e.syms.reserve(e.annots.size());
  for (const auto& a : e.annots) {
    if (a.name.empty()) continue; // 名前の無い（メモだけの）注釈は解決対象にしない
    Symbol s;
    s.addr = a.unslid + e.slide;
    s.mangled = a.name; // 既にユーザが読める名前。displayName はそのまま返す（_Z… でなければデマングルしない）
    e.syms.push_back(std::move(s));
  }
  std::sort(e.syms.begin(), e.syms.end(), [](const Symbol& x, const Symbol& y) { return x.addr < y.addr; });
}

Annotations::Entry* Annotations::find(const Image& img) {
  std::string key = uuidHex(img);
  if (key.empty()) return nullptr;
  auto it = byUuid_.find(key);
  if (it == byUuid_.end() || !it->second.loaded) return nullptr;
  return &it->second;
}

// mu_ 保持前提。stat して未読／mtime・サイズ変化／slide 変化があれば読み直す。変化したら true。
static bool statFile(const std::string& path, time_t& mt, off_t& sz) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) { mt = 0; sz = -1; return false; }
  mt = st.st_mtime; sz = st.st_size; return true;
}

Annotations::Entry& Annotations::getOrLoad(const Image& img) {
  std::string key = uuidHex(img); // 呼び出し側が UUID 有りを保証
  Entry& e = byUuid_[key];
  if (e.path.empty()) e.path = pathFor(img);
  time_t mt = 0; off_t sz = -1;
  bool exists = statFile(e.path, mt, sz);
  // mtime は 1 秒粒度なので、サイズも併せて見て同一秒内の手編集も拾う。
  bool changed = (!e.loaded) || (e.mtime != mt) || (e.size != (long)sz) || (e.slide != img.slide);
  if (changed) {
    std::vector<Annot> na;
    if (exists) parseFile(e.path, na);
    e.annots = std::move(na);
    e.mtime = mt;
    e.size = (long)sz;
    e.slide = img.slide;
    e.loaded = true;
    rebuildSyms(e);
  }
  return e;
}

bool Annotations::refresh(const Image& img) {
  std::lock_guard<std::mutex> lk(mu_);
  if (uuidHex(img).empty()) return false;
  std::string key = uuidHex(img);
  auto it = byUuid_.find(key);
  // 既読でファイルに変化が無ければ false を返したいので、変化判定を自前で見る
  std::string path = (it != byUuid_.end() && !it->second.path.empty()) ? it->second.path : pathFor(img);
  time_t mt = 0; off_t sz = -1; statFile(path, mt, sz);
  bool before = (it != byUuid_.end() && it->second.loaded);
  bool unchanged = before && it->second.mtime == mt && it->second.size == (long)sz && it->second.slide == img.slide;
  if (unchanged) return false;
  getOrLoad(img);
  return true;
}

std::vector<const Symbol*> Annotations::byName(const Image& img, const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<const Symbol*> hits;
  Entry* e = find(img);
  if (!e) return hits;
  for (const auto& s : e->syms) if (s.mangled == name) hits.push_back(&s);
  return hits;
}

const Symbol* Annotations::nearestSym(const Image& img, uint64_t addr) {
  std::lock_guard<std::mutex> lk(mu_);
  Entry* e = find(img);
  if (!e || e->syms.empty()) return nullptr;
  auto it = std::upper_bound(e->syms.begin(), e->syms.end(), addr,
                             [](uint64_t a, const Symbol& s) { return a < s.addr; });
  if (it == e->syms.begin()) return nullptr;
  --it;
  return &*it;
}

const Annot* Annotations::atExact(const Image& img, uint64_t addr) {
  std::lock_guard<std::mutex> lk(mu_);
  Entry* e = find(img);
  if (!e) return nullptr;
  uint64_t unslid = addr - e->slide;
  auto it = std::lower_bound(e->annots.begin(), e->annots.end(), unslid,
                             [](const Annot& a, uint64_t v) { return a.unslid < v; });
  if (it != e->annots.end() && it->unslid == unslid) return &*it;
  return nullptr;
}

std::vector<Annot> Annotations::list(const Image& img) {
  std::lock_guard<std::mutex> lk(mu_);
  Entry* e = find(img);
  if (!e) return {};
  return e->annots; // コピーを返す（表示用）
}

bool Annotations::writeFile(Entry& e, const Image& img, std::string& err) {
  std::string path = pathFor(img);
  if (path.empty()) { err = "UUID の無いイメージには注釈を保存できません"; return false; }
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) { err = "注釈ファイルを書けません: " + path; return false; }
  fputs("# ta2debug annotations v1  (手編集できます / editable by hand)\n", f);
  fputs("# このファイルは 1 つの Mach-O イメージ（UUID 一致）の注釈です。\n", f);
  fputs("# 形式: <16進アドレス(unslid)> <TAB> <名前> <TAB> <メモ>\n", f);
  fputs("#   アドレスは slide を抜いた静的アドレス（Mach-O 上の vmaddr）。再実行や ASLR を跨いでも有効。\n", f);
  fputs("#   名前・メモは省略可。ta2debug の ann/memo/unann でも編集できます（自由記述の # コメント行は再書込で消えます）。\n", f);
  fprintf(f, "# uuid=%s\n", uuidHex(img).c_str());
  fprintf(f, "# path=%s\n\n", img.path.c_str());
  bool ok = true;
  for (const auto& a : e.annots) {
    if (a.name.empty() && a.memo.empty()) continue; // 空の注釈は書かない
    int r;
    if (!a.memo.empty())
      r = fprintf(f, "0x%llx\t%s\t%s\n", (unsigned long long)a.unslid, a.name.c_str(), a.memo.c_str());
    else
      r = fprintf(f, "0x%llx\t%s\n", (unsigned long long)a.unslid, a.name.c_str());
    if (r < 0) { ok = false; break; }
  }
  if (fclose(f) != 0) ok = false;
  if (!ok) { ::remove(tmp.c_str()); err = "注釈ファイルの書込に失敗しました"; return false; }
  if (rename(tmp.c_str(), path.c_str()) != 0) { ::remove(tmp.c_str()); err = "注釈ファイルの置換に失敗しました"; return false; }
  chownToInvoker(path); // sudo 実行時はユーザ所有へ（手編集できるように）
  // 自分の書込で mtime を更新しておく（直後の refresh で無駄に読み直さない）
  e.path = path;
  time_t mt = 0; off_t sz = -1; statFile(path, mt, sz); e.mtime = mt; e.loaded = true;
  return true;
}

// addr（実行時）の注釈を取り出す／無ければ作る共通処理。mu_ 保持前提。
static Annot& annotAt(std::vector<Annot>& annots, uint64_t unslid, bool& created) {
  auto it = std::lower_bound(annots.begin(), annots.end(), unslid,
                             [](const Annot& a, uint64_t v) { return a.unslid < v; });
  if (it != annots.end() && it->unslid == unslid) { created = false; return *it; }
  created = true;
  Annot a; a.unslid = unslid;
  it = annots.insert(it, std::move(a)); // 昇順を保って挿入
  return *it;
}

bool Annotations::setName(const Image& img, uint64_t addr, const std::string& name, std::string& err) {
  std::lock_guard<std::mutex> lk(mu_);
  if (uuidHex(img).empty()) { err = "UUID の無いイメージには注釈を保存できません"; return false; }
  Entry& e = getOrLoad(img);
  uint64_t unslid = addr - img.slide;
  bool created = false;
  Annot& a = annotAt(e.annots, unslid, created);
  a.name = name; // 名前のみ変更（メモは温存）
  // 名前・メモとも空になったら削除
  if (a.name.empty() && a.memo.empty()) {
    e.annots.erase(std::remove_if(e.annots.begin(), e.annots.end(),
                   [unslid](const Annot& x) { return x.unslid == unslid; }), e.annots.end());
  }
  rebuildSyms(e);
  return writeFile(e, img, err);
}

bool Annotations::setMemo(const Image& img, uint64_t addr, const std::string& memo, std::string& err) {
  std::lock_guard<std::mutex> lk(mu_);
  if (uuidHex(img).empty()) { err = "UUID の無いイメージには注釈を保存できません"; return false; }
  Entry& e = getOrLoad(img);
  uint64_t unslid = addr - img.slide;
  bool created = false;
  Annot& a = annotAt(e.annots, unslid, created);
  a.memo = memo; // メモのみ変更（名前は温存）
  if (a.name.empty() && a.memo.empty()) {
    e.annots.erase(std::remove_if(e.annots.begin(), e.annots.end(),
                   [unslid](const Annot& x) { return x.unslid == unslid; }), e.annots.end());
  }
  rebuildSyms(e);
  return writeFile(e, img, err);
}

bool Annotations::remove(const Image& img, uint64_t addr, std::string& err) {
  std::lock_guard<std::mutex> lk(mu_);
  if (uuidHex(img).empty()) { err = "UUID の無いイメージには注釈を保存できません"; return false; }
  Entry& e = getOrLoad(img);
  uint64_t unslid = addr - img.slide;
  size_t before = e.annots.size();
  e.annots.erase(std::remove_if(e.annots.begin(), e.annots.end(),
                 [unslid](const Annot& x) { return x.unslid == unslid; }), e.annots.end());
  if (e.annots.size() == before) { err = "そのアドレスに注釈はありません"; return false; }
  rebuildSyms(e);
  return writeFile(e, img, err);
}

} // namespace ta2
