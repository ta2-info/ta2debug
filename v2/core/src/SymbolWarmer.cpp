#include "core/SymbolWarmer.h"

#include <chrono>
#include <utility>

#include <pthread/qos.h>

namespace ta2 {

void SymbolWarmer::start(std::vector<Image> snap, Progress onProgress) {
  if (!enabled_.load()) return;         // バッチ等では先読みしない
  if (started_.exchange(true)) return;  // 一度だけ起動
  stop_ = false; done_ = 0; total_ = (int)snap.size(); active_ = true;
  if (onProgress) onProgress(0, total_.load(), true); // 開始を即描画に反映
  warmer_ = std::thread([this, snap = std::move(snap), onProgress = std::move(onProgress)]() {
    // 低優先度で走らせ、UI を邪魔せず CPU ファンも回しすぎないようにする（省電力寄り）。
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    auto last = std::chrono::steady_clock::now();
    for (const auto& im : snap) {
      if (stop_.load()) break;
      symbols_.forImage(im); // メモリ＋UUID ディスクキャッシュを温める（Symbols 側で mutex 保護）
      done_.fetch_add(1);
      auto now = std::chrono::steady_clock::now();
      if (now - last > std::chrono::milliseconds(80)) { // 間引いて再描画
        last = now;
        if (onProgress) onProgress(done_.load(), total_.load(), true);
      }
    }
    active_ = false;
    if (onProgress) onProgress(done_.load(), total_.load(), false); // 完了（表示から消す）
  });
}

void SymbolWarmer::stop() {
  stop_ = true;
  if (warmer_.joinable()) warmer_.join();
  active_ = false;
  started_ = false; // 再アタッチ時にまた先読みできるように
}

} // namespace ta2
