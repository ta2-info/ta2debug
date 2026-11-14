#include "core/ExitMonitor.h"

#include <cerrno>
#include <utility>

#include <unistd.h>
#include <sys/event.h>

namespace ta2 {

void ExitMonitor::start(pid_t pid, std::function<void()> onExit) {
  stop(); // 二重起動を避ける（前回の監視が残っていれば畳む）
  kq_ = kqueue();
  if (kq_ < 0) return;
  struct kevent ev[2];
  // 対象 pid の終了を監視。ident=1 の EVFILT_USER は停止要求用の起こし口。
  EV_SET(&ev[0], pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, nullptr);
  EV_SET(&ev[1], 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  kevent(kq_, ev, 2, nullptr, 0, nullptr);
  int kq = kq_;
  monitor_ = std::thread([kq, onExit = std::move(onExit)] {
    for (;;) {
      struct kevent out;
      int n = kevent(kq, nullptr, 0, &out, 1, nullptr);
      if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
      if (out.filter == EVFILT_USER) break; // detach による停止要求
      if (out.filter == EVFILT_PROC && (out.fflags & NOTE_EXIT)) {
        if (onExit) onExit();
        break;
      }
    }
  });
}

void ExitMonitor::stop() {
  if (kq_ >= 0) {
    struct kevent trig; // 監視スレッドを起こして抜けさせる
    EV_SET(&trig, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(kq_, &trig, 1, nullptr, 0, nullptr);
  }
  if (monitor_.joinable()) monitor_.join();
  if (kq_ >= 0) { close(kq_); kq_ = -1; }
}

} // namespace ta2
