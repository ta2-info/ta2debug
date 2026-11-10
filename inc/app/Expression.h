#pragma once

// 式評価（再帰下降）。アドレス・値を受け取るコマンドが使う。
//   式   := 項 (('+'|'-') 項)*
//   項   := 単項 (('*'|'/') 単項)*
//   単項 := ('+'|'-') 単項 | 値
//   値   := 数値 | レジスタ | 名前 | '(' 式 ')'
//   数値 : 0x… 16進 / それ以外 10進
//   レジスタ : $ か % + レジスタ名（名前集合は Arch が決める。停止中のみ値を持つ）
//   名前 : ライブラリ名!シンボル | ライブラリ名! | シンボル | lib
// ライブラリ名省略時は メイン実行ファイルから探す（将来 uselib を前段に入れる）。
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ta2 {

class Session;

class Expression {
public:
  explicit Expression(Session& s) : session_(s) {}
  // 評価して out に結果。失敗時は err にメッセージ（文字位置つき）。
  bool eval(const std::string& input, uint64_t& out, std::string& err);

  // 全イメージ横断のシンボル探索（重い）を中断可能・進捗つきにする。別スレッドから評価する際に設定する。
  //   cancel: 非 null かつ true になったら探索を打ち切り "中止しました" で失敗する。
  //   progress(done,total): イメージ単位の進捗（UI 再描画のトリガ）。省略可。
  // 設定しなければ従来どおり（中断なし・進捗なし）の同期探索。
  void setAsync(std::atomic<bool>* cancel, std::function<void(int, int)> progress) {
    cancel_ = cancel; progress_ = std::move(progress);
  }

private:
  bool parseExpr(uint64_t& v);
  bool parseTerm(uint64_t& v);
  bool parseUnary(uint64_t& v);
  bool parsePrimary(uint64_t& v);
  bool parseNumber(uint64_t& v);
  bool parseRegister(uint64_t& v);
  bool parseName(uint64_t& v);
  void skipSpaces();
  bool fail(const std::string& msg);

  Session& session_;
  std::string input_;
  size_t pos_ = 0;
  std::string error_;
  std::atomic<bool>* cancel_ = nullptr;          // 非 null で探索中断要求を監視
  std::function<void(int, int)> progress_;       // 全イメージ探索の進捗通知（省略可）
};

} // namespace ta2
