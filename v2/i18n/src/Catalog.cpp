#include "i18n/Catalog.h"

#include <unordered_map>

namespace ta2 {
namespace {

// キー → MsgId（Messages.def 由来、1 回だけ構築）。空キー（None）は入れない。
const std::unordered_map<std::string, MsgId>& keyToId() {
  static const std::unordered_map<std::string, MsgId> m = [] {
    std::unordered_map<std::string, MsgId> mm;
#define TA2_MSG(id, key, ja) do { const char* k = key; if (k && k[0]) mm.emplace(k, MsgId::id); } while (0);
#include "base/Messages.def"
#undef TA2_MSG
    return mm;
  }();
  return m;
}

// 文字列前後の空白（半角スペース/タブ）を落とす。
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

// .msg 値のエスケープ（\n \t \\）を実文字へ。{{ }} は差し込み時に処理するのでここでは触らない。
std::string unescape(const std::string& v) {
  std::string out; out.reserve(v.size());
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char n = v[i + 1];
      if (n == 'n') { out += '\n'; i++; continue; }
      if (n == 't') { out += '\t'; i++; continue; }
      if (n == '\\') { out += '\\'; i++; continue; }
    }
    out += v[i];
  }
  return out;
}

} // namespace

bool msgIdForKey(const std::string& key, MsgId& out) {
  auto it = keyToId().find(key);
  if (it == keyToId().end()) return false;
  out = it->second;
  return true;
}

std::set<std::string> placeholderNames(const std::string& tmpl) {
  std::set<std::string> names;
  for (size_t i = 0; i < tmpl.size(); i++) {
    if (tmpl[i] == '{') {
      if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { i++; continue; } // {{ はリテラル
      size_t j = i + 1;
      while (j < tmpl.size() && tmpl[j] != '}') j++;
      if (j < tmpl.size()) {
        std::string inside = tmpl.substr(i + 1, j - i - 1);
        size_t colon = inside.find(':');
        names.insert(colon == std::string::npos ? inside : inside.substr(0, colon));
        i = j;
      }
    } else if (tmpl[i] == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
      i++; // }} はリテラル
    }
  }
  return names;
}

Catalog::Catalog() {
  static const char* const kJa[] = {
#define TA2_MSG(id, key, ja) ja,
#include "base/Messages.def"
#undef TA2_MSG
  };
  tmpl_.assign(kJa, kJa + sizeof(kJa) / sizeof(kJa[0]));
}

Catalog::Report Catalog::overlay(const std::string& msgText) {
  Report rep;
  size_t pos = 0;
  while (pos < msgText.size()) {
    size_t nl = msgText.find('\n', pos);
    std::string line = msgText.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? msgText.size() : nl + 1;
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    std::string val = unescape(trim(line.substr(eq + 1)));
    MsgId id;
    if (!msgIdForKey(key, id)) { rep.unknownKeys.push_back(key); continue; }
    size_t idx = static_cast<size_t>(id);
    if (idx >= tmpl_.size()) { rep.unknownKeys.push_back(key); continue; }
    // プレースホルダ集合が既定とずれていたら記録（翻訳者のミス検出。差し込みは name 基準なので動く）。
    if (placeholderNames(tmpl_[idx]) != placeholderNames(val)) rep.placeholderMismatch.push_back(key);
    tmpl_[idx] = std::move(val);
    rep.applied++;
  }
  return rep;
}

const std::string& Catalog::templateFor(MsgId id) const {
  size_t idx = static_cast<size_t>(id);
  return idx < tmpl_.size() ? tmpl_[idx] : empty_;
}

} // namespace ta2
