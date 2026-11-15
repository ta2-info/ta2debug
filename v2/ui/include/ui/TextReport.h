#pragma once

// L3 Presentation（ta2ui）: エンジンの構造化スナップショット（Types.h）を「表示行」に整形する。
// 設計書 §4.4 / §8.5。TUI のオーバレイとバッチ出力が共用する。ここは素のテキスト（ANSI 無し）を
// 作る。色付け・桁揃え・ペイン配置は各フロントエンドの仕事。文は Translator で翻訳して差し込む。
// SymbolRef / ExamineResult などの整形は L1 の formatDescribe 等を使えるものは使う。
#include <string>
#include <vector>

#include "engine/Debugger.h"
#include "engine/CommandInterpreter.h"
#include "engine/Types.h"
#include "i18n/Translator.h"

namespace ta2 {
namespace ui {

// 構造化結果 → 行。Translator を保持して Msg を翻訳する。Debugger は補助クエリ（describe 等）用。
class TextReport {
public:
  TextReport(const Translator& tr, engine::Debugger& dbg) : tr_(tr), dbg_(dbg) {}

  const Translator& translator() const { return tr_; }
  std::string tr(const Msg& m) const { return tr_.tr(m); }

  // コマンド結果（Posted 完了）: メッセージ（＋アドレス記述）を 1〜2 行で。
  std::vector<std::string> commandResult(const engine::CommandResult& r) const;

  // 停止レポート: "stopped @ ..."＋[bp/watch]、src、理由＋notes、backtrace。
  std::vector<std::string> stopReport(const engine::StopInfo& si,
                                      const std::vector<engine::FrameView>& frames) const;
  // 停止理由の 1 行（例外は "⚠ "＋ヒント、syscall、auto）。無ければ空。
  std::string stopReason(const engine::StopInfo& si) const;
  // バックトレース 1 フレーム（"0x… image off func+0xNN  ← 注記"）。
  std::string frame(const engine::FrameView& f) const;
  std::vector<std::string> backtrace(const std::vector<engine::FrameView>& frames) const;

  // 参照系
  std::vector<std::string> registers(const engine::RegistersSnapshot& rs) const;
  // ベクタレジスタ（want: 1 本の名前。空なら全部。未対応／取得不可／該当なしも行で返す）。
  std::vector<std::string> vectorRegisters(const engine::VectorRegisters& vr, const std::string& want) const;
  std::vector<std::string> debugRegisters(const engine::DebugRegisters& dr) const;
  // syscall 観測（:scwatch）／停止キャッチ（:scstop）の状況。同じ status を視点で出し分ける。
  std::vector<std::string> syscallWatch(const engine::SyscallWatchStatus& st) const;
  std::vector<std::string> syscallStop(const engine::SyscallWatchStatus& st) const;
  std::vector<std::string> syscallNames(const engine::SyscallNameList& ns) const;
  std::vector<std::string> breakpoints(const std::vector<engine::BreakpointInfo>& bps) const;
  std::vector<std::string> images(const std::vector<engine::ImageInfo>& imgs) const;
  std::vector<std::string> segments(const engine::SegmentList& sl) const;
  std::vector<std::string> symbols(const engine::SymbolPage& sp) const;
  std::vector<std::string> annotations(const engine::AnnotationList& al) const;
  std::vector<std::string> examine(const ExamineResult& ex) const; // x / dis（Classic/Values/Strings/Insns）
  std::vector<std::string> help() const;                           // コマンド表から生成

private:
  const Translator& tr_;
  engine::Debugger& dbg_;
};

} // namespace ui
} // namespace ta2
