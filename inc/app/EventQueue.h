#pragma once

// スレッド間イベントの受け渡し（入力スレッド・例外スレッド → メインスレッド）。
// メインスレッドだけが取り出して処理し、描画もメインに集約する（直列化）。
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>

namespace ta2 {

struct Event {
  enum Kind { Key, Stopped, TargetExited, Progress, JobDone, Resize, Mouse } kind;
  // Progress: 進捗更新で再描画を促す（シンボル先読み・シンボル探索の進捗）
  // JobDone : 非同期ジョブ（シンボル式の評価）が完了。メインスレッドが結果を適用する。
  // Resize  : 端末サイズ変更（SIGWINCH）。メインスレッドが render() で全画面を描き直す。
  // Mouse   : マウス入力（SGR 1006）。入力スレッドが列をパースして座標とボタンを積む。
  int key = 0;          // Kind::Key のときのバイト。Kind::Mouse のときはボタンコード。
  int mx = 0, my = 0;   // Kind::Mouse のときの 1 始まり桁/行
  bool press = false;   // Kind::Mouse のとき true=押下(M) / false=解放(m)
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
