#pragma once

// L1 Core（ta2core）。v1 app/Expression との差（設計書 §4.2）:
//  - Session& ではなく狭いインタフェース EvalContext に依存する（L1 が上位を知らない）。
//  - エラーは Msg（err.expr.*）。文字位置つきは ErrExprAt{error=<入れ子 Msg>, pos=<1 始まり>}。
// 文法・探索順（既定ライブラリ → 主実行ファイル → 全イメージ横断）は v1 のまま。

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

#include "base/Msg.h"
#include "core/ImageList.h"
#include "core/Symbols.h"

namespace ta2 {

// 式評価が必要とする最小の文脈。TargetSession が実装する（L2 のジョブは包んで cancel / progress を
// 差し替えられる）。
class EvalContext {
public:
  virtual ~EvalContext() = default;
  // 対象が停止中か（レジスタ参照の可否）。
  virtual bool evalStopped() = 0;
  // 停止スレッドのレジスタを名前（小文字）で読む。名前集合は Arch が決める。不明な名前は false。
  virtual bool evalReadRegister(const std::string& loweredName, uint64_t& out) = 0;
  virtual ImageList& evalImages() = 0;
  virtual Symbols& evalSymbols() = 0;
  // 既定ライブラリ（uselib。未設定／見つからなければ主実行ファイル）。無ければ nullptr。
  virtual const Image* evalDefaultLibImage() = 0;
  // 全イメージ横断探索の中断要求と進捗（イメージ単位）。既定は「中断なし・進捗なし」。
  // Expression::setAsync が設定されていればそちらが優先される。
  virtual bool evalCancelRequested() { return false; }
  virtual void evalProgress(int /*done*/, int /*total*/) {}
};

class Expression {
public:
  explicit Expression(EvalContext& ctx) : ctx_(ctx) {}
  // 評価して out に結果。失敗時は err にメッセージ（文字位置つき）。
  bool eval(const std::string& input, uint64_t& out, Msg& err);

  // 全イメージ横断のシンボル探索（重い）を中断可能・進捗つきにする。別スレッドから評価する際に設定する。
  //   cancel: 非 null かつ true になったら探索を打ち切り err.expr.cancelled で失敗する。
  //   progress(done,total): イメージ単位の進捗（UI 再描画のトリガ）。省略可。
  // 設定しなければ EvalContext の evalCancelRequested / evalProgress（既定は中断なし・進捗なし）。
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
  bool fail(Msg msg);
  bool cancelled() const;
  void progress(int done, int total);

  EvalContext& ctx_;
  std::string input_;
  size_t pos_ = 0;
  Msg error_;
  std::atomic<bool>* cancel_ = nullptr;          // 非 null で探索中断要求を監視
  std::function<void(int, int)> progress_;       // 全イメージ探索の進捗通知（省略可）
};

} // namespace ta2
