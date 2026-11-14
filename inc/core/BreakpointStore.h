#pragma once

// ブレイクポイントの永続化（UUID 別テキスト）。デバッガ終了を跨いで復元するための保存先。
//  - 保存は明示操作（w キー）でのみ行う。無操作の終了では既存ファイルを一切変更しない（勝手に消さない）。
//  - 復元は起動（:run / 初回起動）時に自動で読み、同じアドレスへ張り直す。
//  - sudo 実行でも作成ファイルは呼び出しユーザ所有へ戻す（手編集できるように。Annotations と同方針）。
// アドレスは unslid（slide を抜いた静的アドレス）で、イメージ UUID ごとに持つので ASLR/再実行に強い。
// Mach-O はアーキ非依存なので Arch 依存なし。保存先は Annotations と同じ Application Support 配下の breakpoints。
#include <cstdint>
#include <string>
#include <vector>

namespace ta2 {

// 1 本のブレイクの永続表現。所属イメージ UUID＋unslid でアーキ・ASLR 非依存に復元できる。
struct BpRecord {
  std::string imageUuid; // 大文字 32 桁（所属イメージ）
  uint64_t unslid = 0;   // 実行時アドレス = unslid + そのイメージの slide
  int kind = 0;          // 0=ソフト / 1=HW 実行 / 2=HW ウォッチ
  bool enabled = true;
  int watchSize = 0;              // kind==2 のとき 1/2/4/8
  bool onRead = false, onWrite = false; // kind==2 の監視アクセス種別
};

namespace BreakpointStore {
// ~/Library/Application Support/ta2debug/breakpoints（親ごと作成・chown）。HOME 不明なら空。
std::string dir();
// そのプログラム（主実行ファイル UUID）の保存ファイル <dir>/<UUID>.t2b。UUID 空なら ""。
std::string pathForUuid(const std::string& mainUuid);
// recs を保存（原子的に置換）。成功で true。ファイル所有はユーザへ戻す。
bool save(const std::string& mainUuid, const std::vector<BpRecord>& recs, std::string& err);
// 保存ファイルを読む。無ければ false（out は空）。
bool load(const std::string& mainUuid, std::vector<BpRecord>& out);
} // namespace BreakpointStore

} // namespace ta2
