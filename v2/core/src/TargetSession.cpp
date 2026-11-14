#include "core/TargetSession.h"

#include <cstdio>
#include <vector>

#include <mach/mach_vm.h>
#include <mach-o/loader.h>

namespace ta2 {

TargetSession::TargetSession(SessionSink& sink)
  : sink_(sink),
    bps_(target_),
    images_(target_),
    symbols_(target_),
    dyld_(target_, bps_),
    stepper_(target_, bps_, es_, dyld_, sink),
    scmon_(target_, stepper_, [this](uint64_t addr) { return symbolName(addr); }),
    warmer_(symbols_) {
  symbols_.setAnnotations(&annots_); // ユーザ注釈をシンボル解決に重ねる
}

TargetSession::~TargetSession() {
  detach();
}

bool TargetSession::installExceptionHandler(Msg& err) {
  return es_.install(target_.task(),
                     [this](thread_act_t t, exception_type_t e, const int64_t* c, int n) {
                       return stepper_.onException(t, e, c, n);
                     },
                     err);
}

bool TargetSession::attach(pid_t pid, Msg& err) {
  if (!target_.attach(pid, err)) return false;
  stepper_.prepare();
  if (!installExceptionHandler(err)) {
    target_.detach();
    return false;
  }
  images_.reload();
  exitMon_.start(target_.pid(), [this] { sink_.onTargetExited(); });
  warmSymbols(); // 全ライブラリのシンボルを裏で先読み・キャッシュ
  return true;
}

bool TargetSession::launch(const std::string& path, char* const argv[], Msg& err) {
  if (!target_.launch(path, argv, err)) return false;
  stepper_.prepare();
  if (!installExceptionHandler(err)) {
    target_.detach();
    return false;
  }
  exitMon_.start(target_.pid(), [this] { sink_.onTargetExited(); });
  // START_SUSPENDED の時点では dyld が走る前で、まだどのイメージも読み込まれていない
  //（PC は __dyld_start）。そのまま止めてもシンボルでブレイクできないので、メイン実行ファイルの
  // エントリ点に一時ブレイクを置いて走らせ、dyld 完了後（全シンボル解決済み）にそこで止める。
  uint64_t entry = findEntryPoint();
  if (entry) {
    stepper_.armEntryStop(entry);
  } else {
    // エントリ不明（LC_UNIXTHREAD のみ等）: 従来どおり現在地（dyld エントリ）で停止
    images_.reload();
    stepper_.enterInitialStop();
  }
  return true;
}

// メモリ領域を走査して MH_EXECUTE（メイン実行ファイル）の Mach-O ヘッダを見つけ、
// LC_MAIN の entryoff から実行時エントリ（= ヘッダ先頭 + entryoff）を返す。見つからなければ 0。
uint64_t TargetSession::findEntryPoint() {
  task_t task = target_.task();
  mach_vm_address_t addr = 1;
  for (int scanned = 0; scanned < 4000; scanned++) {
    mach_vm_size_t sz = 0; natural_t depth = 0;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(task, &addr, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
    if (info.is_submap) { depth++; continue; }
    uint64_t base = addr;
    addr += sz; // 次の領域へ（header 読めなくても進む）
    mach_header_64 mh = {};
    if (!target_.readMemory(base, &mh, sizeof(mh))) continue;
    if (mh.magic != MH_MAGIC_64 || mh.filetype != MH_EXECUTE) continue;
    std::vector<uint8_t> lc(mh.sizeofcmds);
    if (!target_.readMemory(base + sizeof(mh), lc.data(), lc.size())) continue;
    size_t off = 0;
    for (uint32_t i = 0; i < mh.ncmds && off + sizeof(load_command) <= lc.size(); i++) {
      auto* c = reinterpret_cast<load_command*>(lc.data() + off);
      if (c->cmdsize == 0) break;
      if (c->cmd == LC_MAIN) {
        auto* e = reinterpret_cast<entry_point_command*>(lc.data() + off);
        return base + e->entryoff; // entryoff はヘッダ先頭からのオフセット（スライド込み）
      }
      off += c->cmdsize;
    }
    return 0; // メインは見つけたが LC_MAIN 無し（LC_UNIXTHREAD のみ）
  }
  return 0;
}

void TargetSession::detach() {
  scmon_.stop();   // consumer スレッドを target 破棄前に必ず止める
  warmer_.stop();  // target 破棄前に warming スレッドを必ず止める
  if (!target_.isAttached()) return;
  exitMon_.stop();
  stepper_.releaseForDetach(); // 停止の種類に応じて全ブレイクを外し、再開できる状態にする
  es_.uninstall();
  target_.detach();
  dyld_.reset(); // 次の起動/アタッチで通知フックを張り直す
}

const Image* TargetSession::defaultLibImage() {
  if (!defaultLib_.empty()) { const Image* im = images_.byName(defaultLib_); if (im) return im; }
  return images_.mainExecutable();
}

DwarfLines* TargetSession::linesForImage(const Image& img) {
  std::string key;
  if (img.hasUuid) { char h[33]; for (int i = 0; i < 16; i++) snprintf(h + i*2, 3, "%02x", img.uuid[i]); key.assign(h, 32); }
  else key = img.path;
  auto it = lineCache_.find(key);
  if (it == lineCache_.end()) {
    DwarfLines& dl = lineCache_[key];
    dl.loadForImage(img.path, img.uuid, img.slide); // 失敗しても loaded()=false で覚えておく（再試行しない）
    return dl.loaded() ? &dl : nullptr;
  }
  return it->second.loaded() ? &it->second : nullptr;
}

bool TargetSession::sourceLineFor(uint64_t addr, SourceLoc& out) {
  const Image* im = images_.byAddress(addr);
  if (!im) return false;
  DwarfLines* dl = linesForImage(*im);
  if (!dl) return false;
  if (!dl->lookup(addr, out)) return false;
  out.text = dl->sourceText(out.path, out.line); // 読めればソース行テキストも
  return true;
}

void TargetSession::warmSymbols() {
  if (!target_.isAttached()) return;
  if (!warmer_.canStart()) return; // 無効化中・起動済みなら一覧のコピーもしない（停止のたびに呼ばれる）
  // 一覧をコピーして渡す（スレッド間でリストを共有しない）。有効/無効・一度だけ起動は SymbolWarmer 側。
  warmer_.start(images_.images(), [this](int done, int total, bool active) {
    sink_.onWarmProgress(done, total, active);
  });
}

SymbolRef TargetSession::describe(uint64_t addr) {
  return resolveAddress(images_, symbols_, &annots_, addr);
}

std::string TargetSession::symbolName(uint64_t addr) {
  return formatSymbolName(resolveAddress(images_, symbols_, nullptr, addr, /*wantMemo=*/false));
}

bool TargetSession::syscallAtStop(SyscallCall& out) {
  if (!stepper_.stopped()) return false;
  return decodeSyscallAt(target_, bps_, stepper_.stopPc(), stepper_.stopState().data(), out);
}

bool TargetSession::evalReadRegister(const std::string& loweredName, uint64_t& out) {
  return target_.arch().readRegisterByName(stepper_.stopState().data(), loweredName, out);
}

} // namespace ta2
