#include "engine/JobRunner.h"

#include <exception>
#include <utility>

namespace ta2 {
namespace engine {

bool JobRunner::start(RequestId request, const std::string& expr, Apply apply) {
  if (running_.load()) return false;
  if (thread_.joinable()) thread_.join(); // 念のため（前回の finish で join 済みのはず）
  cancel_ = false; showing_ = false; done_ = 0; total_ = 0;
  request_ = request;
  apply_ = std::move(apply);
  ok_ = false; value_ = 0; error_ = Msg();
  running_ = true;
  thread_ = std::thread([this, expr]() {
    Expression ex(ctx_);
    ex.setAsync(&cancel_, [this](int done, int total) {
      done_.store(done); total_.store(total);
      showing_.store(true); // 重い探索に入った → 進捗表示を有効化
      InboxItem it;
      it.kind = InboxItem::JobProgress;
      it.done = done; it.total = total;
      inbox_.push(std::move(it));
    });
    uint64_t v = 0; Msg e;
    bool ok = false;
    try { ok = ex.eval(expr, v, e); }
    catch (const std::exception& x) { ok = false; e = Msg::of(MsgId::ErrJobInternal).str("detail", x.what()); }
    catch (...) { ok = false; e = Msg::of(MsgId::ErrJobInternalUnknown); }
    ok_ = ok; value_ = v; error_ = std::move(e);
    InboxItem it;
    it.kind = InboxItem::JobDone;
    inbox_.push(std::move(it)); // エンジンが結果を適用
  });
  return true;
}

void JobRunner::cancel(RequestId request) {
  if (!running_.load()) return;
  if (request != 0 && request != request_.load()) return;
  cancel_.store(true);
}

JobRunner::Outcome JobRunner::finish() {
  Outcome o;
  if (thread_.joinable()) thread_.join();
  o.request = request_.load();
  o.ok = ok_; o.value = value_; o.error = error_;
  o.cancelled = !ok_ && cancel_.load();
  o.apply = std::move(apply_);
  apply_ = nullptr;
  showing_ = false;
  running_ = false;
  return o;
}

JobStatus JobRunner::status() const {
  JobStatus s;
  s.running = running_.load();
  s.showing = s.running && showing_.load();
  s.done = done_.load(); s.total = total_.load();
  s.request = s.running ? request_.load() : 0;
  return s;
}

void JobRunner::shutdown() {
  cancel_.store(true);
  if (thread_.joinable()) thread_.join();
  apply_ = nullptr;
  showing_ = false;
  running_ = false;
}

} // namespace engine
} // namespace ta2
