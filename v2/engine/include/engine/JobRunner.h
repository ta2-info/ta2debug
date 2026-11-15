#pragma once

// L2（ta2engine）: キャンセル可能・進捗つきの非同期ジョブ（シンボル式の評価）。由来は v1 main.cpp の
// evalAsync / g_job*（設計書 §4.3）。同時に 1 本。走行中、エンジンは他の状態変更コマンドを受けない
// （v1 の g_jobRunning ガードと同じ規則。ガードそのものは Debugger::handleCommand）。
//
// 配線: Expression::setAsync(&cancel, progress) を使う（v1 と同じ）。ワーカは評価だけを行い、
// 進捗は受信箱へ JobProgress、完了は JobDone を積む。結果の適用（apply）はエンジン側が JobDone を
// 処理するときに finish() で受け取って実行する（v1 のメインスレッド適用と同じ直列化）。
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "base/Msg.h"
#include "core/Expression.h"
#include "engine/EngineInbox.h"
#include "engine/Types.h"

namespace ta2 {
namespace engine {

class JobRunner {
public:
  // 評価結果の適用（エンジン側で実行）。ok=false のとき err は式エラー（err.expr.* / err.job.*）。
  using Apply = std::function<void(bool ok, uint64_t value, const Msg& err)>;

  JobRunner(EvalContext& ctx, EngineInbox& inbox) : ctx_(ctx), inbox_(inbox) {}
  ~JobRunner() { shutdown(); }

  JobRunner(const JobRunner&) = delete;
  JobRunner& operator=(const JobRunner&) = delete;

  // ワーカ走行中か（JobDone をエンジンが処理し終えるまで true）。
  bool running() const { return running_.load(); }
  // expr の評価を別スレッドで始める。すでに走行中なら false（何もしない）。
  bool start(RequestId request, const std::string& expr, Apply apply);
  // 中止要求（v1 の ESC）。request が 0 か走行中のジョブと一致すれば効く。
  void cancel(RequestId request = 0);

  struct Outcome {
    RequestId request = 0;
    bool ok = false;
    uint64_t value = 0;
    Msg error;
    bool cancelled = false; // 失敗かつ中止要求あり（v1: 「中止しました」。apply は呼ばない）
    Apply apply;
  };
  // JobDone を受けたエンジンが呼ぶ。ワーカを join して結果を取り出し、走行中フラグを下ろす。
  Outcome finish();

  JobStatus status() const;
  // 中止して join する（~Debugger 用。結果は捨てる）。冪等。
  void shutdown();

private:
  EvalContext& ctx_;
  EngineInbox& inbox_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> showing_{false}; // 進捗表示を出す段階か（＝重い全イメージ探索に入った）
  std::atomic<int> done_{0};
  std::atomic<int> total_{0};
  std::atomic<RequestId> request_{0};
  // ワーカの結果。JobDone 後（join 後）にエンジンだけが読むので同期は不要（v1 と同じ取り決め）。
  bool ok_ = false;
  uint64_t value_ = 0;
  Msg error_;
  Apply apply_;
};

} // namespace engine
} // namespace ta2
