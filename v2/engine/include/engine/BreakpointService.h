#pragma once

// L2（ta2engine）: ブレイクポイントの上位操作。由来は v1 main.cpp の bpAddKind / bpRebuild /
// toggleBreakAtCursor の中核 / saveBreakpoints / restoreBreakpoints / applyOneBp /
// applyPendingBreakpoints / doRestart の引き継ぎ部（設計書 §4.3）。
// 種別変換・移動・ウォッチ条件変更（失敗時の巻き戻し込み）、HW 同期、永続化、復元、
// 保留（dlopen 待ち）管理を持つ。物理層・ユーザ層そのものは L1 の Breakpoints。
// 状態を変えるスレッド（エンジン）からのみ呼ぶ。
#include <cstdint>
#include <string>
#include <vector>

#include "base/Msg.h"
#include "core/BreakpointStore.h"
#include "core/Breakpoints.h"
#include "core/TargetSession.h"
#include "engine/Types.h"

namespace ta2 {
namespace engine {

class BreakpointService {
public:
  explicit BreakpointService(TargetSession& session) : s_(session) {}

  static BpKind kindOf(const Breakpoint& b) { return b.watch ? BpKind::Watch : b.hw ? BpKind::HwExec : BpKind::Soft; }
  // Image の UUID を大文字 32 桁で（永続化のキー）。無ければ空。
  static std::string uuidHexOf(const Image& img);

  // 指定アドレスに種別・有効状態・ウォッチ条件で張る（v1 bpAddKind）。成功時は HW 同期まで行う。
  // 失敗（HW スロット不足・メモリに書けない）で nullptr。
  const Breakpoint* addKind(BpKind kind, uint64_t addr, bool enabled, int wsize, bool onR, bool onW);
  // old を消して newKind / newAddr / … で張り直す。失敗したら元の種別・アドレスへ戻す（best-effort）。
  // どちらの場合も張り直したブレイクは一覧の末尾に付き、その id を newId に返す（戻せなければ -1）。
  bool rebuild(const Breakpoint& old, BpKind newKind, uint64_t newAddr, int wsize, bool onR, bool onW,
               int& newId);
  // id 指定の削除（HW なら同期）。無ければ false。
  bool remove(int id);
  // 有効⇄無効。nowEnabled に切替後の状態。無い id・切替失敗で false（found で区別）。
  bool toggleEnabled(int id, bool& nowEnabled, bool& found);

  // ---- 永続化（w キー / :run 時の自動復元） ----
  // 今のユーザブレイクを主実行ファイル UUID のファイルへ保存する（v1 saveBreakpoints）。
  // 結果の文（成功・失敗とも）を msg に入れ、成功で true。
  bool save(Msg& msg);
  // 保存ファイルを読み、ロード済みイメージへ張り直す。未ロード分は保留へ回す。
  // 返り値=復元件数（ファイル無し等は -1）。v1 restoreBreakpoints。
  int restoreSaved();
  // 保留中（dlopen 待ち）のうち、イメージがロード済みになったものを張る。返り値=今回張れた件数。
  int applyPending();
  size_t pendingCount() const { return pending_.size(); }
  void clearPending() { pending_.clear(); }

  // ---- :restart の引き継ぎ ----
  // saved（再起動前に控えたユーザブレイク）を同一アドレスへ張り直す（種別と有効状態を保つ）。返り値=張れた件数。
  int reapply(const std::vector<Breakpoint>& saved);

private:
  // 1 件の保存レコードを、所属イメージがロード済みなら張る。張れたら true。未ロードなら false。
  bool applyOne(const BpRecord& r, bool& usedHw);

  TargetSession& s_;
  std::vector<BpRecord> pending_; // 所属イメージが未ロードで張れていない保存ブレイク
};

} // namespace engine
} // namespace ta2
