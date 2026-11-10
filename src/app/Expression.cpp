#include "app/Expression.h"

#include <cctype>
#include <cstdio>

#include "core/Session.h"

namespace ta2 {

namespace {
bool isModuleChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-' || c == '+';
}
bool isSymbolChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$' || c == ':';
}
} // namespace

bool Expression::eval(const std::string& input, uint64_t& out, std::string& err) {
  input_ = input;
  pos_ = 0;
  error_.clear();
  out = 0;
  skipSpaces();
  if (pos_ >= input_.size()) { err = "式がない"; return false; }
  uint64_t v = 0;
  if (!parseExpr(v)) { err = error_; return false; }
  skipSpaces();
  if (pos_ < input_.size()) { err = "式の後に余分な文字がある (" + std::to_string(pos_ + 1) + " 文字目)"; return false; }
  out = v;
  return true;
}

void Expression::skipSpaces() {
  while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t')) pos_++;
}

bool Expression::fail(const std::string& msg) {
  if (error_.empty()) error_ = msg + " (" + std::to_string(pos_ + 1) + " 文字目)";
  return false;
}

bool Expression::parseExpr(uint64_t& v) {
  if (!parseTerm(v)) return false;
  for (;;) {
    skipSpaces();
    if (pos_ >= input_.size()) return true;
    char op = input_[pos_];
    if (op != '+' && op != '-') return true;
    pos_++;
    uint64_t rhs = 0;
    if (!parseTerm(rhs)) return false;
    v = (op == '+') ? (v + rhs) : (v - rhs);
  }
}

bool Expression::parseTerm(uint64_t& v) {
  if (!parseUnary(v)) return false;
  for (;;) {
    skipSpaces();
    if (pos_ >= input_.size()) return true;
    char op = input_[pos_];
    if (op != '*' && op != '/') return true;
    pos_++;
    uint64_t rhs = 0;
    if (!parseUnary(rhs)) return false;
    if (op == '/') {
      if (rhs == 0) return fail("0 で割れない");
      v = v / rhs;
    } else {
      v = v * rhs;
    }
  }
}

bool Expression::parseUnary(uint64_t& v) {
  skipSpaces();
  if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
    char op = input_[pos_];
    pos_++;
    if (!parseUnary(v)) return false;
    if (op == '-') v = (uint64_t)(-(int64_t)v);
    return true;
  }
  return parsePrimary(v);
}

bool Expression::parsePrimary(uint64_t& v) {
  skipSpaces();
  if (pos_ >= input_.size()) return fail("値がない");
  char c = input_[pos_];

  if (c == '(') {
    size_t open = pos_;
    pos_++;
    if (!parseExpr(v)) return false;
    skipSpaces();
    if (pos_ >= input_.size() || input_[pos_] != ')') { pos_ = open; return fail("括弧が閉じていない"); }
    pos_++;
    return true;
  }
  if (std::isdigit((unsigned char)c)) return parseNumber(v);
  if (c == '$' || c == '%') return parseRegister(v);
  if (std::isalpha((unsigned char)c) || c == '_') return parseName(v);
  if (c == ')') return fail("対応する '(' がない ')'");
  return fail(std::string("解釈できない文字 '") + c + "'");
}

bool Expression::parseNumber(uint64_t& v) {
  size_t start = pos_;
  v = 0;
  if (input_[pos_] == '0' && pos_ + 1 < input_.size() && (input_[pos_ + 1] == 'x' || input_[pos_ + 1] == 'X')) {
    pos_ += 2;
    size_t digits = 0;
    while (pos_ < input_.size() && std::isxdigit((unsigned char)input_[pos_])) {
      char d = (char)std::tolower((unsigned char)input_[pos_]);
      v = v * 16 + (uint64_t)((d >= 'a') ? (d - 'a' + 10) : (d - '0'));
      pos_++; digits++;
    }
    if (digits == 0) { pos_ = start; return fail("0x の後に 16 進数がない"); }
  } else {
    while (pos_ < input_.size() && std::isdigit((unsigned char)input_[pos_])) {
      v = v * 10 + (uint64_t)(input_[pos_] - '0');
      pos_++;
    }
  }
  if (pos_ < input_.size() && (std::isalpha((unsigned char)input_[pos_]) || input_[pos_] == '_'))
    return fail("数値として解釈できない");
  return true;
}

bool Expression::parseRegister(uint64_t& v) {
  size_t start = pos_;
  pos_++; // $ or %
  std::string name;
  while (pos_ < input_.size() && std::isalnum((unsigned char)input_[pos_]))
    name += (char)std::tolower((unsigned char)input_[pos_++]);
  if (name.empty()) { pos_ = start; return fail("レジスタ名がない"); }
  if (!session_.stopped()) { pos_ = start; return fail("レジスタは停止中のみ参照できる"); }
  if (session_.arch().readRegisterByName(session_.stopState().data(), name, v)) return true;
  pos_ = start;
  return fail("不明なレジスタ '" + name + "'");
}

bool Expression::parseName(uint64_t& v) {
  size_t start = pos_;

  // '!' までをライブラリ名とみなせるか（'.' '-' '+' を含められる）
  std::string module;
  bool hasModule = false;
  {
    size_t p = pos_;
    while (p < input_.size() && isModuleChar(input_[p])) p++;
    if (p < input_.size() && input_[p] == '!') { module = input_.substr(pos_, p - pos_); hasModule = true; pos_ = p + 1; }
  }

  std::string name;
  while (pos_ < input_.size() && isSymbolChar(input_[pos_])) name += input_[pos_++];

  // 複数候補（別アドレス）のとき、各候補の名前＋アドレスを列挙したメッセージを作る。
  // バレ名でオーバーロードや複数定義に当たったとき、どれを選ぶか分かるように。
  auto ambiguousMsg = [&](const std::vector<const Symbol*>& hits) -> std::string {
    std::string m = "シンボル '" + name + "' は複数あります: ";
    for (size_t i = 0; i < hits.size() && i < 8; i++) {
      if (i) m += " / ";
      char a[24]; snprintf(a, sizeof(a), "@0x%llx", (unsigned long long)hits[i]->addr);
      m += session_.symbols().displayName(*hits[i]) + " " + a;
    }
    if (hits.size() > 8) m += " …";
    m += "。アドレスか『ライブラリ名!名前』で指定してください";
    return m;
  };

  if (hasModule) {
    const Image* img = session_.images().byName(module);
    if (!img) { pos_ = start; return fail("ライブラリ '" + module + "' が見つからない"); }
    if (name.empty()) { v = img->loadAddr; return true; }
    auto hits = session_.symbols().byName(*img, name);
    if (hits.empty()) { pos_ = start; return fail("シンボル '" + name + "' が " + img->name + " に無い"); }
    for (auto* s : hits) if (s->addr != hits[0]->addr) { pos_ = start; return fail(ambiguousMsg(hits)); }
    v = hits[0]->addr;
    return true;
  }

  if (name.empty()) return fail("名前がない");

  // ライブラリ名省略時: 既定ライブラリ（uselib、未設定なら主実行ファイル）→ 主実行ファイル の順に探す。
  const Image* defImg = session_.defaultLibImage();
  const Image* mainImg = session_.images().mainExecutable();
  if (name == "lib") {
    const Image* base = defImg ? defImg : mainImg;
    if (!base) { pos_ = start; return fail("既定ライブラリ／主実行ファイルが不明"); }
    v = base->loadAddr;
    return true;
  }
  if (!defImg && !mainImg) { pos_ = start; return fail("'" + name + "': 探すイメージが無い（アタッチ済み？）"); }
  std::vector<const Symbol*> hits;
  if (defImg) hits = session_.symbols().byName(*defImg, name);
  if (hits.empty() && mainImg && mainImg != defImg) hits = session_.symbols().byName(*mainImg, name);
  if (!hits.empty()) {
    for (auto* s : hits) if (s->addr != hits[0]->addr) { pos_ = start; return fail(ambiguousMsg(hits)); }
    v = hits[0]->addr; return true;
  }

  // 既定ライブラリ／主実行ファイルに無ければ、ロード済み全イメージを横断して探す
  //（ライブラリ名を省略してもシンボルを引けるように。シンボルキャッシュで高速化・UUIDで更新検知）。
  std::vector<std::pair<const Image*, uint64_t>> found;
  // 一覧はコピーして走る（この探索は別スレッドで実行され得るため、停止イベント等での
  // ImageList 更新と競合させない）。&im はこの関数内のエラーメッセージ生成まで有効。
  std::vector<Image> imgs = session_.images().images();
  int total = (int)imgs.size(), idx = 0;
  for (const auto& im : imgs) {
    // キャンセル（ESC）要求があれば即中断。進捗はイメージ単位で通知（UI が固まらないように）。
    if (cancel_ && cancel_->load()) { pos_ = start; return fail("中止しました"); }
    if (progress_) progress_(idx, total);
    auto h = session_.symbols().byName(im, name);
    if (!h.empty()) found.push_back({&im, h[0]->addr});
    idx++;
  }
  if (progress_) progress_(total, total);
  if (!found.empty()) {
    bool allSame = true;
    for (auto& f : found) if (f.second != found[0].second) { allSame = false; break; }
    if (allSame) { v = found[0].second; return true; } // 1 ライブラリ、または同一アドレス
    // 複数ライブラリに別アドレスで存在 → 明示指定（ライブラリ名!）を促す
    std::string libs;
    for (size_t i = 0; i < found.size() && i < 6; i++) { if (i) libs += ", "; libs += found[i].first->name; }
    if (found.size() > 6) libs += " …";
    pos_ = start; return fail("シンボル '" + name + "' は複数ライブラリにあります（" + libs + "）。『ライブラリ名!" + name + "』で指定してください");
  }

  std::string where = defImg ? defImg->name : std::string("?");
  if (mainImg && mainImg != defImg) where += " / " + mainImg->name;
  pos_ = start; return fail("シンボル '" + name + "' が見つからない（" + where + " ほか全イメージを探した。ライブラリ名!" + name + " も試せます）");
}

} // namespace ta2
