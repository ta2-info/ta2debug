#include "i18n/Translator.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <variant>

namespace ta2 {

std::string Translator::formatArg(const MsgArg& a, const std::string& spec) const {
  // 入れ子 Msg は再帰的に翻訳（spec は無視）。
  if (auto p = std::get_if<std::shared_ptr<Msg>>(&a.value))
    return *p ? tr(**p) : std::string();

  // spec を解析: [0][幅][x|X|d][q]。例 "016x" / "d" / "q"。
  bool zero = false; int width = 0; char type = 0; bool quote = false;
  size_t i = 0;
  if (i < spec.size() && spec[i] == '0') { zero = true; i++; }
  while (i < spec.size() && std::isdigit((unsigned char)spec[i])) { width = width * 10 + (spec[i] - '0'); i++; }
  for (; i < spec.size(); i++) { char c = spec[i];
    if (c == 'x' || c == 'X') type = 'x'; else if (c == 'd') type = 'd'; else if (c == 'q') quote = true; }

  char b[96];
  if (auto u = std::get_if<uint64_t>(&a.value)) {
    bool asHex = (type == 'x') || (type == 0 && a.hex);
    if (asHex) {
      if (zero && width > 0) snprintf(b, sizeof(b), "0x%0*llx", width, (unsigned long long)*u);
      else                   snprintf(b, sizeof(b), "0x%llx", (unsigned long long)*u);
    } else {
      snprintf(b, sizeof(b), "%llu", (unsigned long long)*u);
    }
    return b;
  }
  if (auto n = std::get_if<int64_t>(&a.value)) {
    snprintf(b, sizeof(b), "%lld", (long long)*n);
    return b;
  }
  if (auto s = std::get_if<std::string>(&a.value))
    return quote ? ("\"" + *s + "\"") : *s;
  return std::string();
}

std::string Translator::subst(const std::string& tmpl, const Msg& m) const {
  std::string out; out.reserve(tmpl.size() + 16);
  for (size_t i = 0; i < tmpl.size(); i++) {
    char c = tmpl[i];
    if (c == '{') {
      if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { out += '{'; i++; continue; } // {{ → {
      size_t j = i + 1;
      while (j < tmpl.size() && tmpl[j] != '}') j++;
      if (j >= tmpl.size()) { out += c; continue; }            // 閉じない { はそのまま
      std::string inside = tmpl.substr(i + 1, j - i - 1);
      size_t colon = inside.find(':');
      std::string name = (colon == std::string::npos) ? inside : inside.substr(0, colon);
      std::string spec = (colon == std::string::npos) ? std::string() : inside.substr(colon + 1);
      const MsgArg* arg = m.find(name.c_str());
      if (arg) out += formatArg(*arg, spec);
      else     out += "{" + inside + "}";                      // 引数が無ければそのまま残す（検査で検出）
      i = j;
    } else if (c == '}') {
      if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') { out += '}'; i++; continue; } // }} → }
      out += c;
    } else {
      out += c;
    }
  }
  return out;
}

std::string Translator::tr(const Msg& m) const {
  if (m.empty()) return std::string();
  const std::string& tmpl = cat_.templateFor(m.id);
  if (tmpl.empty()) return std::string(msgKey(m.id)); // 既定も空なら安定キーを返す（通常起きない）
  return subst(tmpl, m);
}

std::string detectLocaleLang() {
  const char* v = nullptr;
  for (const char* e : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* x = std::getenv(e);
    if (x && x[0]) { v = x; break; }
  }
  if (!v) return "ja";
  std::string s = v;
  size_t cut = s.find_first_of("_.@");
  std::string lang = (cut == std::string::npos) ? s : s.substr(0, cut);
  if (lang.empty() || lang == "C" || lang == "POSIX") return "ja";
  return lang;
}

} // namespace ta2
