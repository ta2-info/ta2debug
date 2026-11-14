#pragma once

// L1 Core（ta2core）: 全イメージのシンボル先読み（warming）。由来は v1 Session::warmSymbols /
// stopSymbolWarming と warm* 状態（設計書 §4.2）。
// ロード済み全イメージのシンボルをバックグラウンド（低優先度）で読み、UUID キャッシュ（Symbols）へ
// 保存する。進捗は開始時・80ms 間引きの途中経過・完了時に onProgress で知らせる（v1 の
// Event::Progress。上位は SessionSink::onWarmProgress へ流す）。
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "core/ImageList.h"
#include "core/Symbols.h"

namespace ta2 {

class SymbolWarmer {
public:
  // done / total / active。先読みスレッド（と start を呼んだスレッド）から呼ばれる。
  using Progress = std::function<void(int done, int total, bool active)>;

  explicit SymbolWarmer(Symbols& symbols) : symbols_(symbols) {}
  ~SymbolWarmer() { stop(); }

  SymbolWarmer(const SymbolWarmer&) = delete;
  SymbolWarmer& operator=(const SymbolWarmer&) = delete;

  // 先読みの有効/無効（バッチは一度きりなので無効化してオーバーヘッドを無くす）。
  void setEnabled(bool on) { enabled_ = on; }

  // images（一覧のコピー。スレッド間でリストを共有しない）の先読みを始める。一度だけ起動する
  // （すでに起動済み・無効化中なら何もしない）。stop() 後はまた起動できる。
  void start(std::vector<Image> images, Progress onProgress);
  // start() が実際に起動する状態か（有効で未起動）。呼び出し側が一覧のコピーを省くための事前確認。
  bool canStart() const { return enabled_.load() && !started_.load(); }
  // 先読みスレッドを止めて join する（冪等）。Target / Symbols の破棄前に必ず呼ぶこと。
  void stop();

  // 進捗（表示用）。warming 中か / 済んだ数 / 総数。
  bool active() const { return active_.load(); }
  int done() const { return done_.load(); }
  int total() const { return total_.load(); }

private:
  Symbols& symbols_;
  std::thread warmer_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> enabled_{true};  // 先読みを行うか（バッチでは false）
  std::atomic<bool> active_{false};  // 先読みスレッドが走行中か（表示用）
  std::atomic<int> done_{0};         // 先読み済みイメージ数
  std::atomic<int> total_{0};        // 先読み対象の総数
};

} // namespace ta2
