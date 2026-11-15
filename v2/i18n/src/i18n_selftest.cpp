// ta2i18n セルフテスト（設計書 §8.3 の「検査」）。カタログの既定（日本語）が全 MsgId に入っているか、
// Translator の差し込み（hex/数値/文字列/入れ子/書式/エスケープ/{{}}）が正しいか、overlay が未知キーと
// プレースホルダ不一致を検出するか、ロケール判定が環境変数どおりかを確かめる。全て通れば 0。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "base/Msg.h"
#include "i18n/Catalog.h"
#include "i18n/Translator.h"

using namespace ta2;

static int g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& got = "", const std::string& want = "") {
  if (ok) { printf("  ok   %s\n", what.c_str()); return; }
  g_fail++;
  printf("  FAIL %s\n", what.c_str());
  if (!got.empty() || !want.empty()) printf("       got=[%s] want=[%s]\n", got.c_str(), want.c_str());
}
static void eq(const std::string& got, const std::string& want, const std::string& what) {
  check(got == want, what, got, want);
}

int main() {
  printf("i18n_selftest\n");

  // 1) 既定カタログ: 全 MsgId（None 以外）に非空テンプレートがある
  Catalog def;
  int emptyCount = 0;
  for (uint16_t i = 1; i < (uint16_t)MsgId::Count_; i++) {
    MsgId id = (MsgId)i;
    if (def.templateFor(id).empty()) { emptyCount++; if (emptyCount <= 5) printf("       empty template: %s\n", msgKey(id)); }
  }
  check(emptyCount == 0, "すべての MsgId に既定（日本語）テンプレートがある");

  Translator tr(def);

  // 2) 代表的な翻訳（既定カタログ）
  eq(tr.tr(Msg::of(MsgId::ErrExprDivZero)), "0 で割れない", "引数なし");
  eq(tr.tr(Msg::of(MsgId::ErrExprBadChar).str("ch", "$")), "解釈できない文字 '$'", "文字列差し込み {ch}");
  eq(tr.tr(Msg::of(MsgId::BpSet).num("id", 1).hex("addr", 0x100003f46ULL)),
     "#1 ブレイク設定 0x100003f46", "数値 {id} + hex {addr}");
  // 入れ子: err.expr.at の {error} に別の Msg、{pos} に数値
  eq(tr.tr(Msg::of(MsgId::ErrExprAt).sub("error", Msg::of(MsgId::ErrExprDivZero)).num("pos", 5)),
     "0 で割れない (5 文字目)", "入れ子 Msg {error} + {pos}");
  {
    std::string s = tr.tr(Msg::of(MsgId::ErrTaskForPid).str("detail", "ZZZ"));
    check(s.find("ZZZ") != std::string::npos && s.find("{detail}") == std::string::npos, "{detail} 差し込み");
  }
  // 空 Msg は空文字
  eq(tr.tr(Msg()), "", "空 Msg");

  // 3) overlay（別言語の試作想定）＋ 書式指定・エスケープ・{{}}
  {
    Catalog c;
    Catalog::Report rep = c.overlay(
      "# test overlay\n"
      "bp.set = id={id:d} @ {addr:016x}\n"             // 書式: 10進・ゼロ詰め16進
      "err.expr.bad_char = bad {ch:q}\n"               // :q で引用
      "err.expr.div_zero = line1\\nline2\n"            // \\n エスケープ
      "err.expr.empty = brace {{x}} ok\n"             // {{ }} リテラル
    );
    Translator t(c, "test");
    check(rep.unknownKeys.empty(), "overlay: 既知キーのみ（未知キーなし）");
    check(rep.placeholderMismatch.empty(), "overlay: プレースホルダ一致");
    eq(t.tr(Msg::of(MsgId::BpSet).num("id", 1).hex("addr", 0x100003f46ULL)),
       "id=1 @ 0x0000000100003f46", "書式 {id:d} / {addr:016x}");
    eq(t.tr(Msg::of(MsgId::ErrExprBadChar).str("ch", "$")), "bad \"$\"", "書式 {ch:q}");
    eq(t.tr(Msg::of(MsgId::ErrExprDivZero)), "line1\nline2", "エスケープ \\n");
    eq(t.tr(Msg::of(MsgId::ErrExprEmpty)), "brace {x} ok", "{{ }} リテラル");
  }

  // 4) overlay の検出: 未知キー・プレースホルダ不一致
  {
    Catalog c;
    Catalog::Report rep = c.overlay(
      "no.such.key = whatever\n"
      "bp.set = only {id}\n"                            // addr が欠けている
    );
    bool unknown = false; for (auto& k : rep.unknownKeys) if (k == "no.such.key") unknown = true;
    bool mism = false; for (auto& k : rep.placeholderMismatch) if (k == "bp.set") mism = true;
    check(unknown, "overlay: 未知キー no.such.key を検出");
    check(mism, "overlay: プレースホルダ不一致 bp.set を検出");
  }

  // 4.5) 埋め込み英語カタログ（i-6）: loadCatalog("en") で全キーが英訳され、未知キー・
  //      プレースホルダ不一致がゼロ（＝「未訳キーゼロ」の完了条件）。日本語とも別物であること。
  {
    setenv("TA2DEBUG_I18N_DIR", "/nonexistent-ta2debug-i18n", 1); // ユーザ上書きを拾わない
    Catalog::Report rep;
    Catalog en = loadCatalog("en", &rep);
    unsetenv("TA2DEBUG_I18N_DIR");
    check(rep.applied == (int)MsgId::Count_ - 1, "en.msg: 全 MsgId を適用（未訳キーゼロ）",
          std::to_string(rep.applied), std::to_string((int)MsgId::Count_ - 1));
    check(rep.unknownKeys.empty(), "en.msg: 未知キーなし");
    check(rep.placeholderMismatch.empty(), "en.msg: プレースホルダ一致");
    int sameAsJa = 0;
    for (uint16_t i = 1; i < (uint16_t)MsgId::Count_; i++) {
      MsgId id = (MsgId)i;
      if (en.templateFor(id).empty()) { check(false, std::string("en 空テンプレート: ") + msgKey(id)); break; }
      if (en.templateFor(id) == def.templateFor(id)) sameAsJa++;
    }
    // 記号・固定トークンのみの少数キー（note_sep 等）は英日同一になり得るので緩め（< 10）に。
    check(sameAsJa < 10, "en.msg: ほぼ全キーが日本語と別文", std::to_string(sameAsJa));
    Translator ten(en, "en");
    eq(ten.tr(Msg::of(MsgId::ErrExprDivZero)), "division by zero", "en: err.expr.div_zero");
    eq(ten.tr(Msg::of(MsgId::BpSet).num("id", 1).hex("addr", 0x100003f46ULL)),
       "#1 breakpoint set 0x100003f46", "en: bp.set の差し込み");
  }

  // 5) ロケール判定（環境変数）
  {
    unsetenv("LC_ALL"); unsetenv("LC_MESSAGES"); unsetenv("LANG");
    eq(detectLocaleLang(), "ja", "ロケール未設定 → ja");
    setenv("LANG", "en_US.UTF-8", 1);
    eq(detectLocaleLang(), "en", "LANG=en_US.UTF-8 → en");
    setenv("LC_ALL", "ja_JP.UTF-8", 1);           // LC_ALL が優先
    eq(detectLocaleLang(), "ja", "LC_ALL=ja_JP.UTF-8 が LANG より優先 → ja");
    setenv("LC_ALL", "C", 1);
    eq(detectLocaleLang(), "ja", "LC_ALL=C → ja（既定）");
    unsetenv("LC_ALL"); unsetenv("LANG");
  }

  printf(g_fail == 0 ? "ALL OK\n" : "FAILED (%d)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
