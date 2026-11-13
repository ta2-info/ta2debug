#pragma once

// スレッド間イベントの受け渡し（入力スレッド・例外スレッド → メインスレッド）。
// メインスレッドだけが取り出して処理し、描画もメインに集約する（直列化）。
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>

namespace ta2 {

struct Event {
  enum Kind { Key, Stopped, TargetExited, Progress, JobDone, Resize } kind;
  // Progress: 進捗更新で再描画を促す（シンボル先読み・シンボル探索の進捗）
  // JobDone : 非同期ジョブ（シンボル式の評価）が完了。メインスレッドが結果を適用する。
  // Resize  : 端末サイズ変更（SIGWINCH）。メインスレッドが render() で全画面を描き直す。
  int key = 0;          // Kind::Key のときのバイト
};

class EventQueue {
public:
  void push(const Event& e) {
    { std::lock_guard<std::mutex> lk(m_); q_.push(e); }
    cv_.notify_one();
  }
  Event pop() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return !q_.empty(); });
    Event e = q_.front(); q_.pop();
    return e;
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  std::queue<Event> q_;
};

} // namespace ta2
