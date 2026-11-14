#pragma once

// L1 Core（ta2core）: dyld のイメージ通知フック。由来は v1 Session::ensureDyldNotifier /
// dyldNotifyAddr_ / stopIsImageLoad_ の判定（設計書 §4.2）。
// dyld の all_image_infos.notification（イメージ増減のたびに dyld が呼ぶ関数）へ内部ソフトブレイクを
// 1 本置き、そこへの命中を「内部停止」として識別できるようにする（gdb / lldb と同じ手法）。
// 命中後の扱い（イメージ更新 + 保留ブレイク装着 + 即 cont）は上位（L2 StopProcessor）の仕事。
#include <cstdint>

#include "core/Breakpoints.h"
#include "platform/Target.h"

namespace ta2 {

class DyldNotifier {
public:
  DyldNotifier(Target& target, Breakpoints& bps) : target_(target), bps_(bps) {}

  // 通知関数へ内部ソフトブレイク（物理層のみ。ユーザ一覧には入れない）を置く。
  // 停止中（タスク suspend 済み）に呼ぶこと。冪等。すでに設置済み、または通知アドレスが
  // 取れなければ何もしない。
  void ensure();

  // addr が設置済みの通知フックか。Stepper が例外スレッドから呼んで内部停止を見分ける
  // （同じアドレスにユーザブレイクがある場合の優先は Stepper 側で判定する）。
  bool isNotifierAddr(uint64_t addr) const { return addr_ != 0 && addr == addr_; }
  uint64_t address() const { return addr_; }

  // detach 時: 次の起動 / アタッチで通知フックを張り直すため、覚えたアドレスを忘れる
  // （物理ブレイクは Breakpoints::removeAll が外す）。
  void reset() { addr_ = 0; }

private:
  Target& target_;
  Breakpoints& bps_;
  uint64_t addr_ = 0; // 通知フックのアドレス（内部キャッチポイント）。0=未設置
};

} // namespace ta2
