#include "core/Expression.h"

#include <cctype>
#include <cstdio>

#include <utility>
#include <vector>

namespace ta2 {

namespace {
bool isModuleChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-' || c == '+';
}
bool isSymbolChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$' || c == ':';
}
} // namespace

bool Expression::eval(const std::string& input, uint64_t& out, Msg& err) {
  input_ = input;
  pos_ = 0;
  error_ = Msg();
  out = 0;
  skipSpaces();
  if (pos_ >= input_.size()) { err = Msg::of(MsgId::ErrExprEmpty); return false; }
  uint64_t v = 0;
  if (!parseExpr(v)) { err = error_; return false; }
  skipSpaces();
  if (pos_ < input_.size()) { err = Msg::of(MsgId::ErrExprTrailing).num("pos", (int64_t)pos_ + 1); return false; }
  out = v;
  return true;
}

void Expression::skipSpaces() {
  while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t')) pos_++;
}

// 最初のエラーだけを覚える（文字位置つき。v1 の「<文> (N 文字目)」）。
bool Expression::fail(Msg msg) {
  if (error_.empty())
    error_ = Msg::of(MsgId::ErrExprAt).sub("error", std::move(msg)).num("pos", (int64_t)pos_ + 1);
  return false;
}

bool Expression::cancelled() const {
  if (cancel_) return cancel_->load();
  return ctx_.evalCancelRequested();
}

void Expression::progress(int done, int total) {
  if (progress_) progress_(done, total);
  else ctx_.evalProgress(done, total);
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
      if (rhs == 0) return fail(Msg::of(MsgId::ErrExprDivZero));
      v = v / rhs;
    } else {
      v = v * rhs;
    }
  }
}

bool Expression::parseUnary(uint64_t& v) {
  skipSpaces();
  // C 風ポインタ参照（デリファレンス）: オペランド先頭の '*' は「そのアドレスが指す 8 バイト値」。
  // 二項の乗算 a*b は parseTerm が parseUnary の後ろで拾うので、前置の '*' とは位置で曖昧なく区別。
  // 右結合なので **p（二重参照）や *(p+8) も可。
  if (pos_ < input_.size() && input_[pos_] == '*') {
    pos_++;
    uint64_t addr;
    if (!parseUnary(addr)) return false;
    uint64_t val = 0; // ポインタ幅（8 バイト・リトルエンディアン）
    if (!ctx_.evalReadMemory(addr, &val, sizeof(val)))
      return fail(Msg::of(MsgId::ErrMemUnreadable).hex("addr", addr));
    v = val;
    return true;
  }
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
  if (pos_ >= input_.size()) return fail(Msg::of(MsgId::ErrExprNoValue));
  char c = input_[pos_];

  if (c == '(') {
    size_t open = pos_;
    pos_++;
    if (!parseExpr(v)) return false;
    skipSpaces();
    if (pos_ >= input_.size() || input_[pos_] != ')') { pos_ = open; return fail(Msg::of(MsgId::ErrExprUnclosedParen)); }
    pos_++;
    return true;
  }
  if (std::isdigit((unsigned char)c)) return parseNumber(v);
  if (c == '$' || c == '%') return parseRegister(v);
  if (std::isalpha((unsigned char)c) || c == '_') return parseName(v);
  if (c == ')') return fail(Msg::of(MsgId::ErrExprUnmatchedParen));
  return fail(Msg::of(MsgId::ErrExprBadChar).str("ch", std::string(1, c)));
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
    if (digits == 0) { pos_ = start; return fail(Msg::of(MsgId::ErrExprHexDigits)); }
  } else {
    while (pos_ < input_.size() && std::isdigit((unsigned char)input_[pos_])) {
      v = v * 10 + (uint64_t)(input_[pos_] - '0');
      pos_++;
    }
  }
  if (pos_ < input_.size() && (std::isalpha((unsigned char)input_[pos_]) || input_[pos_] == '_'))
    return fail(Msg::of(MsgId::ErrExprBadNumber));
  return true;
}

bool Expression::parseRegister(uint64_t& v) {
  size_t start = pos_;
  pos_++; // $ or %
  std::string name;
  while (pos_ < input_.size() && std::isalnum((unsigned char)input_[pos_]))
    name += (char)std::tolower((unsigned char)input_[pos_++]);
  if (name.empty()) { pos_ = start; return fail(Msg::of(MsgId::ErrExprNoRegName)); }
  if (!ctx_.evalStopped()) { pos_ = start; return fail(Msg::of(MsgId::ErrExprRegNeedsStop)); }
  if (ctx_.evalReadRegister(name, v)) return true;
  pos_ = start;
  return fail(Msg::of(MsgId::ErrExprUnknownReg).str("name", name));
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
  auto ambiguousMsg = [&](const std::vector<const Symbol*>& hits) -> Msg {
    std::string m; // {candidates}: 「表示名 @0x.. / 表示名 @0x.. …」（シンボル名は翻訳対象外）
    for (size_t i = 0; i < hits.size() && i < 8; i++) {
      if (i) m += " / ";
      char a[24]; snprintf(a, sizeof(a), "@0x%llx", (unsigned long long)hits[i]->addr);
      m += ctx_.evalSymbols().displayName(*hits[i]) + " " + a;
    }
    if (hits.size() > 8) m += " …";
    return Msg::of(MsgId::ErrExprAmbiguous).str("name", name).str("candidates", m);
  };

  if (hasModule) {
    const Image* img = ctx_.evalImages().byName(module);
    if (!img) { pos_ = start; return fail(Msg::of(MsgId::ErrExprNoLib).str("lib", module)); }
    if (name.empty()) { v = img->loadAddr; return true; }
    auto hits = ctx_.evalSymbols().byName(*img, name);
    if (hits.empty()) { pos_ = start; return fail(Msg::of(MsgId::ErrExprSymNotInLib).str("name", name).str("lib", img->name)); }
    for (auto* s : hits) if (s->addr != hits[0]->addr) { pos_ = start; return fail(ambiguousMsg(hits)); }
    v = hits[0]->addr;
    return true;
  }

  if (name.empty()) return fail(Msg::of(MsgId::ErrExprNoName));

  // ライブラリ名省略時: 既定ライブラリ（uselib、未設定なら主実行ファイル）→ 主実行ファイル の順に探す。
  const Image* defImg = ctx_.evalDefaultLibImage();
  const Image* mainImg = ctx_.evalImages().mainExecutable();
  if (name == "lib") {
    const Image* base = defImg ? defImg : mainImg;
    if (!base) { pos_ = start; return fail(Msg::of(MsgId::ErrExprNoDefaultLib)); }
    v = base->loadAddr;
    return true;
  }
  if (!defImg && !mainImg) { pos_ = start; return fail(Msg::of(MsgId::ErrExprNoImages).str("name", name)); }
  std::vector<const Symbol*> hits;
  if (defImg) hits = ctx_.evalSymbols().byName(*defImg, name);
  if (hits.empty() && mainImg && mainImg != defImg) hits = ctx_.evalSymbols().byName(*mainImg, name);
  if (!hits.empty()) {
    for (auto* s : hits) if (s->addr != hits[0]->addr) { pos_ = start; return fail(ambiguousMsg(hits)); }
    v = hits[0]->addr; return true;
  }

  // 既定ライブラリ／主実行ファイルに無ければ、ロード済み全イメージを横断して探す
  //（ライブラリ名を省略してもシンボルを引けるように。シンボルキャッシュで高速化・UUIDで更新検知）。
  std::vector<std::pair<const Image*, uint64_t>> found;
  // 一覧はコピーして走る（この探索は別スレッドで実行され得るため、停止イベント等での
  // ImageList 更新と競合させない）。&im はこの関数内のエラーメッセージ生成まで有効。
  std::vector<Image> imgs = ctx_.evalImages().images();
  int total = (int)imgs.size(), idx = 0;
  for (const auto& im : imgs) {
    // キャンセル（ESC）要求があれば即中断。進捗はイメージ単位で通知（UI が固まらないように）。
    if (cancelled()) { pos_ = start; return fail(Msg::of(MsgId::ErrExprCancelled)); }
    progress(idx, total);
    auto h = ctx_.evalSymbols().byName(im, name);
    if (!h.empty()) found.push_back({&im, h[0]->addr});
    idx++;
  }
  progress(total, total);
  if (!found.empty()) {
    bool allSame = true;
    for (auto& f : found) if (f.second != found[0].second) { allSame = false; break; }
    if (allSame) { v = found[0].second; return true; } // 1 ライブラリ、または同一アドレス
    // 複数ライブラリに別アドレスで存在 → 明示指定（ライブラリ名!）を促す
    std::string libs;
    for (size_t i = 0; i < found.size() && i < 6; i++) { if (i) libs += ", "; libs += found[i].first->name; }
    if (found.size() > 6) libs += " …";
    pos_ = start; return fail(Msg::of(MsgId::ErrExprMultiLib).str("name", name).str("libs", libs));
  }

  std::string where = defImg ? defImg->name : std::string("?");
  if (mainImg && mainImg != defImg) where += " / " + mainImg->name;
  pos_ = start; return fail(Msg::of(MsgId::ErrExprSymNotFound).str("name", name).str("where", where));
}

} // namespace ta2
