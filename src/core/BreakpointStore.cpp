#include "core/BreakpointStore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

namespace ta2 {

// sudo 実行時は作成ファイルを呼び出しユーザ（SUDO_UID/GID）所有へ戻す（best-effort）。
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

std::string BreakpointStore::dir() {
  const char* home = getenv("HOME");
  if (!home || !*home) return "";
  std::string a = std::string(home) + "/Library/Application Support";
  mkdir(a.c_str(), 0755);
  std::string b = a + "/ta2debug";
  mkdir(b.c_str(), 0755);
  std::string c = b + "/breakpoints";
  mkdir(c.c_str(), 0755);
  chownToInvoker(b); chownToInvoker(c);
  return c;
}

std::string BreakpointStore::pathForUuid(const std::string& mainUuid) {
  if (mainUuid.empty()) return "";
  std::string d = dir();
  if (d.empty()) return "";
  return d + "/" + mainUuid + ".t2b";
}

static const char* kindName(int k) { return k == 1 ? "hw" : k == 2 ? "watch" : "soft"; }
static const char* rwName(bool r, bool w) { return (r && w) ? "rw" : r ? "r" : w ? "w" : "-"; }

bool BreakpointStore::save(const std::string& mainUuid, const std::vector<BpRecord>& recs, std::string& err) {
  std::string path = pathForUuid(mainUuid);
  if (path.empty()) { err = "UUID の無いプログラムはブレイクを保存できません"; return false; }
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) { err = "保存ファイルを書けません: " + path; return false; }
  fputs("# ta2debug breakpoints v1  (手編集できます / editable by hand)\n", f);
  fputs("# 形式: <イメージUUID> <TAB> <unslid 16進> <TAB> <種類> <TAB> <有効1/0> <TAB> <watchサイズ> <TAB> <r|w|rw|->\n", f);
  fputs("#   種類: soft=ソフトブレイク / hw=HW 実行 / watch=HW ウォッチ。unslid は slide を抜いた静的アドレス。\n", f);
  fputs("#   w キーで保存。無操作の終了ではこのファイルは変更されません。\n\n", f);
  bool ok = true;
  for (const auto& r : recs) {
    if (r.imageUuid.empty()) continue; // 所属イメージ不明は保存しない（復元できないため）
    int n = fprintf(f, "%s\t0x%llx\t%s\t%d\t%d\t%s\n",
                    r.imageUuid.c_str(), (unsigned long long)r.unslid, kindName(r.kind),
                    r.enabled ? 1 : 0, r.watchSize, rwName(r.onRead, r.onWrite));
    if (n < 0) { ok = false; break; }
  }
  if (fclose(f) != 0) ok = false;
  if (!ok) { ::remove(tmp.c_str()); err = "保存ファイルの書込に失敗しました"; return false; }
  if (rename(tmp.c_str(), path.c_str()) != 0) { ::remove(tmp.c_str()); err = "保存ファイルの置換に失敗しました"; return false; }
  chownToInvoker(path);
  return true;
}

// 1 行をパース。TAB 区切り: uuid \t 0xunslid \t kind \t en \t wsize \t rw。足りない列は既定。
static bool parseLine(const std::string& raw, BpRecord& out) {
  std::string line = raw;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  size_t i = 0; while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i >= line.size() || line[i] == '#') return false;

  // TAB で分割
  std::vector<std::string> cols; size_t start = 0;
  for (size_t k = 0; k <= line.size(); k++) {
    if (k == line.size() || line[k] == '\t') { cols.push_back(line.substr(start, k - start)); start = k + 1; }
  }
  if (cols.size() < 2) return false;
  // 前後空白を落とす
  auto trim = [](std::string s) {
    size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t"); return s.substr(a, b - a + 1);
  };
  std::string uuid = trim(cols[0]);
  std::string atok = trim(cols[1]);
  if (uuid.empty() || atok.empty()) return false;
  std::string h = atok;
  if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
  char* end = nullptr;
  unsigned long long v = strtoull(h.c_str(), &end, 16);
  if (end == h.c_str() || (end && *end != '\0')) return false;

  out = BpRecord{};
  out.imageUuid = uuid;
  out.unslid = (uint64_t)v;
  std::string kind = cols.size() > 2 ? trim(cols[2]) : "soft";
  out.kind = (kind == "hw") ? 1 : (kind == "watch") ? 2 : 0;
  out.enabled = cols.size() > 3 ? (trim(cols[3]) != "0") : true;
  out.watchSize = cols.size() > 4 ? atoi(trim(cols[4]).c_str()) : 0;
  std::string rw = cols.size() > 5 ? trim(cols[5]) : "-";
  out.onRead  = (rw.find('r') != std::string::npos);
  out.onWrite = (rw.find('w') != std::string::npos);
  return true;
}

bool BreakpointStore::load(const std::string& mainUuid, std::vector<BpRecord>& out) {
  out.clear();
  std::string path = pathForUuid(mainUuid);
  if (path.empty()) return false;
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  char* buf = nullptr; size_t cap = 0; ssize_t n;
  while ((n = getline(&buf, &cap, f)) != -1) {
    BpRecord r;
    if (parseLine(std::string(buf, (size_t)n), r)) out.push_back(std::move(r));
  }
  free(buf);
  fclose(f);
  return true;
}

} // namespace ta2
