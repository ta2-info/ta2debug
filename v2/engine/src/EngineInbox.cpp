#include "engine/EngineInbox.h"

#include <chrono>
#include <utility>

namespace ta2 {
namespace engine {

void EngineInbox::onRawStop(const ta2::RawStop& stop) {
  InboxItem it;
  it.kind = InboxItem::RawStop;
  it.stop = stop;
  push(std::move(it));
}

void EngineInbox::onTargetExited() {
  InboxItem it;
  it.kind = InboxItem::TargetExited;
  push(std::move(it));
}

void EngineInbox::onWarmProgress(int done, int total, bool active) {
  InboxItem it;
  it.kind = InboxItem::WarmProgress;
  it.done = done; it.total = total; it.active = active;
  push(std::move(it));
}

void EngineInbox::push(InboxItem item) {
  item.generation = generation_.load();
  { std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(item)); }
  cv_.notify_one();
}

void EngineInbox::pushFront(InboxItem item) {
  { std::lock_guard<std::mutex> lk(m_); q_.push_front(std::move(item)); }
  cv_.notify_one();
}

bool EngineInbox::pop(InboxItem& out, int timeoutMs) {
  std::unique_lock<std::mutex> lk(m_);
  if (timeoutMs < 0) cv_.wait(lk, [this] { return !q_.empty(); });
  else if (timeoutMs > 0) cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return !q_.empty(); });
  if (q_.empty()) return false;
  out = std::move(q_.front());
  q_.pop_front();
  return true;
}

} // namespace engine
} // namespace ta2
