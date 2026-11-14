#include "core/DyldNotifier.h"

#include <algorithm>
#include <cstddef>

#include <mach/task_info.h>
#include <mach-o/dyld_images.h>

namespace ta2 {

// dyld の all_image_infos.notification（イメージ増減のたびに dyld が呼ぶ関数）へ内部ソフトブレイクを置く。
// gdb/lldb と同じ手法。以降 dlopen/dlclose でそこへ #BP が来るので、上位が静かに追従できる。
// 停止中（タスク suspend 済み）に呼ぶこと。すでに設置済み、または通知アドレスが取れなければ何もしない。
void DyldNotifier::ensure() {
  if (addr_ && bps_.has(addr_)) return; // 設置済み
  if (!target_.isAttached()) return;
  task_dyld_info_data_t info = {};
  mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
  if (task_info(target_.task(), TASK_DYLD_INFO, (task_info_t)&info, &cnt) != KERN_SUCCESS) return;
  struct dyld_all_image_infos all = {};
  size_t want = std::min(sizeof(all), (size_t)info.all_image_info_size);
  if (want < offsetof(struct dyld_all_image_infos, notification) + sizeof(void*)) return;
  if (!target_.readMemory(info.all_image_info_addr, &all, want)) return;
  uint64_t addr = (uint64_t)all.notification;
  if (!addr) return;                 // dyld が通知関数を公開していない
  if (!bps_.addSoft(addr)) return;   // ここに INT3/BRK を置く（物理層。ユーザ一覧には入れない）
  addr_ = addr;
}

} // namespace ta2
