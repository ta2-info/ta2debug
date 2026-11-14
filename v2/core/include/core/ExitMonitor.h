#pragma once

// L1 Core（ta2core）: 対象プロセスの終了監視（kqueue EVFILT_PROC / NOTE_EXIT）。
// 由来は v1 Session::startExitMonitor / stopExitMonitor（設計書 §4.2）。
// 終了を検知したら onExit を監視スレッドから 1 回呼ぶ（上位は SessionSink::onTargetExited へ流す）。
#include <functional>
#include <thread>

#include <sys/types.h>

namespace ta2 {

class ExitMonitor {
public:
  ExitMonitor() = default;
  ~ExitMonitor() { stop(); }

  ExitMonitor(const ExitMonitor&) = delete;
  ExitMonitor& operator=(const ExitMonitor&) = delete;

  // pid の終了監視を始める。onExit は監視スレッドから呼ばれる（積んで即戻ること）。
  // kqueue を開けなければ何もしない（監視なしで続行。v1 と同じ）。
  void start(pid_t pid, std::function<void()> onExit);
  // 監視スレッドを起こして抜けさせ、join する（冪等）。
  void stop();

private:
  std::thread monitor_;
  int kq_ = -1;
};

} // namespace ta2
