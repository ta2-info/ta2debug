#include "core/Session.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <pthread/qos.h>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <mach/mach_vm.h>
#include <mach/message.h>
#include <mach/port.h>
#include <mach/vm_prot.h>
#include <mach/vm_statistics.h>
#include <mach-o/loader.h>

#include "core/Disassembler.h"

namespace ta2 {

Session::~Session() {
  detach();
}

bool Session::attach(pid_t pid, std::string& err) {
  if (!target_.attach(pid, err)) return false;
  stopState_.resize(target_.arch().threadStateCount() * sizeof(natural_t));
  if (!es_.install(target_.task(),
                   [this](thread_act_t t, exception_type_t e, const int64_t* c, int n) {
                     return onException(t, e, c, n);
                   },
                   err)) {
    target_.detach();
    return false;
  }
  images_.reload();
  startExitMonitor();
  warmSymbols(); // 全ライブラリのシンボルを裏で先読み・キャッシュ
  return true;
}

bool Session::launch(const std::string& path, char* const argv[], std::string& err) {
  if (!target_.launch(path, argv, err)) return false;
  stopState_.resize(target_.arch().threadStateCount() * sizeof(natural_t));
  if (!es_.install(target_.task(),
                   [this](thread_act_t t, exception_type_t e, const int64_t* c, int n) {
                     return onException(t, e, c, n);
                   },
                   err)) {
    target_.detach();
    return false;
  }
  startExitMonitor();
  // START_SUSPENDED の時点では dyld が走る前で、まだどのイメージも読み込まれていない
  //（PC は __dyld_start）。そのまま止めてもシンボルでブレイクできないので、メイン実行ファイルの
  // エントリ点に一時ブレイクを置いて走らせ、dyld 完了後（全シンボル解決済み）にそこで止める。
  uint64_t entry = findEntryPoint();
  if (entry) {
    bps_.addSoft(entry);
    tempValid_ = true; tempAddr_ = entry; // 一回限り（命中で自動除去）
    target_.resume();                     // START_SUSPENDED 分を解除 → entry で onException が止める
  } else {
    // エントリ不明（LC_UNIXTHREAD のみ等）: 従来どおり現在地（dyld エントリ）で停止
    images_.reload();
    auto threads = target_.threads();
    if (!threads.empty()) {
      thread_act_t t = threads[0];
      target_.getThreadState(t, stopState_.data());
      stopPc_ = target_.arch().pc(stopState_.data());
      stopThread_ = t;
      manualStop_ = true;
      lastWatchId_ = -1;
      stopExc_ = 0;
      stopped_ = true;
      queue_.push({Event::Stopped, 0});
    }
  }
  return true;
}

// メモリ領域を走査して MH_EXECUTE（メイン実行ファイル）の Mach-O ヘッダを見つけ、
// LC_MAIN の entryoff から実行時エントリ（= ヘッダ先頭 + entryoff）を返す。見つからなければ 0。
uint64_t Session::findEntryPoint() {
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

void Session::startExitMonitor() {
  monitorKq_ = kqueue();
  if (monitorKq_ < 0) return;
  struct kevent ev[2];
  // 対象 pid の終了を監視。ident=1 の EVFILT_USER は停止要求用の起こし口。
  EV_SET(&ev[0], target_.pid(), EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, nullptr);
  EV_SET(&ev[1], 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  kevent(monitorKq_, ev, 2, nullptr, 0, nullptr);
  int kq = monitorKq_;
  monitor_ = std::thread([this, kq] {
    for (;;) {
      struct kevent out;
      int n = kevent(kq, nullptr, 0, &out, 1, nullptr);
      if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
      if (out.filter == EVFILT_USER) break; // detach による停止要求
      if (out.filter == EVFILT_PROC && (out.fflags & NOTE_EXIT)) {
        queue_.push({Event::TargetExited, 0});
        break;
      }
    }
  });
}

void Session::stopExitMonitor() {
  if (monitorKq_ >= 0) {
    struct kevent trig; // 監視スレッドを起こして抜けさせる
    EV_SET(&trig, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(monitorKq_, &trig, 1, nullptr, 0, nullptr);
  }
  if (monitor_.joinable()) monitor_.join();
  if (monitorKq_ >= 0) { close(monitorKq_); monitorKq_ = -1; }
}

const Image* Session::defaultLibImage() {
  if (!defaultLib_.empty()) { const Image* im = images_.byName(defaultLib_); if (im) return im; }
  return images_.mainExecutable();
}

DwarfLines* Session::linesForImage(const Image& img) {
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

bool Session::sourceLineFor(uint64_t addr, SourceLoc& out) {
  const Image* im = images_.byAddress(addr);
  if (!im) return false;
  DwarfLines* dl = linesForImage(*im);
  if (!dl) return false;
  if (!dl->lookup(addr, out)) return false;
  out.text = dl->sourceText(out.path, out.line); // 読めればソース行テキストも
  return true;
}

void Session::warmSymbols() {
  if (!warmingEnabled_.load()) return;     // バッチ等では先読みしない
  if (!target_.isAttached()) return;
  if (warmStarted_.exchange(true)) return; // 一度だけ起動
  std::vector<Image> snap = images_.images(); // 一覧をコピー（スレッド間でリストを共有しない）
  warmStop_ = false; warmDone_ = 0; warmTotal_ = (int)snap.size(); warmActive_ = true;
  queue_.push({Event::Progress, 0}); // 開始を即描画に反映
  warmer_ = std::thread([this, snap]() {
    // 低優先度で走らせ、UI を邪魔せず CPU ファンも回しすぎないようにする（省電力寄り）。
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    auto last = std::chrono::steady_clock::now();
    for (const auto& im : snap) {
      if (warmStop_.load()) break;
      symbols_.forImage(im); // メモリ＋UUID ディスクキャッシュを温める（Symbols 側で mutex 保護）
      warmDone_.fetch_add(1);
      auto now = std::chrono::steady_clock::now();
      if (now - last > std::chrono::milliseconds(80)) { last = now; queue_.push({Event::Progress, 0}); } // 間引いて再描画
    }
    warmActive_ = false;
    queue_.push({Event::Progress, 0}); // 完了（表示から消す）
  });
}

void Session::stopSymbolWarming() {
  warmStop_ = true;
  if (warmer_.joinable()) warmer_.join();
  warmActive_ = false;
  warmStarted_ = false; // 再アタッチ時にまた先読みできるように
}

std::string Session::describe(uint64_t addr) {
  const Image* img = images_.byAddress(addr);
  if (!img) return std::string();
  char head[64];
  snprintf(head, sizeof(head), "%s 0x%llx", img->name.c_str(),
           (unsigned long long)(addr - img->loadAddr));
  std::string s(head);
  if (img->containsStub(addr)) { // __stubs: 直前シンボルに巻き込まれないよう、指す先を明示
    std::string t = symbols_.stubTarget(*img, addr);
    return s + (t.empty() ? "  [symbol stub]" : ("  symbol stub for: " + t));
  }
  const Symbol* sym = symbols_.byAddress(*img, addr);
  if (sym) {
    char off[32];
    snprintf(off, sizeof(off), "+0x%llx", (unsigned long long)(addr - sym->addr));
    s += "  " + symbols_.displayName(*sym) + off;
  }
  return s;
}

std::string Session::stopReason() const {
  if (stopExc_ == 0) return std::string(); // 通常のブレイク/ステップ停止
  const char* name;
  switch (stopExc_) {
    case EXC_BAD_ACCESS:      name = "EXC_BAD_ACCESS（不正メモリ参照）"; break;
    case EXC_BAD_INSTRUCTION: name = "EXC_BAD_INSTRUCTION（不正命令）";  break;
    case EXC_ARITHMETIC:      name = "EXC_ARITHMETIC（算術例外）";       break;
    default:                  name = "例外";                           break;
  }
  std::string s = name;
  if (stopExc_ == EXC_BAD_ACCESS) {
    const char* kr = (stopCode0_ == KERN_INVALID_ADDRESS)   ? " KERN_INVALID_ADDRESS"
                   : (stopCode0_ == KERN_PROTECTION_FAILURE) ? " KERN_PROTECTION_FAILURE"
                   : nullptr;
    char buf[80];
    if (kr) snprintf(buf, sizeof(buf), " [%s] @0x%llx", kr + 1, (unsigned long long)(uint64_t)stopCode1_);
    else    snprintf(buf, sizeof(buf), " [code=%lld] @0x%llx", (long long)stopCode0_, (unsigned long long)(uint64_t)stopCode1_);
    s += buf;
  } else {
    char buf[48];
    snprintf(buf, sizeof(buf), " [code=%lld]", (long long)stopCode0_);
    s += buf;
  }
  return s;
}

std::string Session::symbolName(uint64_t addr) {
  const Image* img = images_.byAddress(addr);
  if (!img) return std::string();
  if (img->containsStub(addr)) {
    std::string t = symbols_.stubTarget(*img, addr);
    return t.empty() ? (img->name + " [stub]") : (t + " [stub]");
  }
  const Symbol* sym = symbols_.byAddress(*img, addr);
  if (!sym) return img->name;
  uint64_t off = addr - sym->addr;
  const std::string& dn = symbols_.displayName(*sym);
  if (off == 0) return dn;
  char buf[32];
  snprintf(buf, sizeof(buf), "+0x%llx", (unsigned long long)off);
  return dn + buf;
}

namespace {
// システムコール表。sig は引数ごとの型: d=10進, x=16進, p=ポインタ, s=C文字列(参照先), B=バッファ(長さは次の引数)。
struct Scall { int num; const char* name; const char* sig; };
const Scall kScalls[] = {
#ifdef SYS_exit
  {SYS_exit,"exit","d"},
#endif
#ifdef SYS_read
  {SYS_read,"read","dpd"},
#endif
#ifdef SYS_write
  {SYS_write,"write","dBd"},
#endif
#ifdef SYS_open
  {SYS_open,"open","sOx"},
#endif
#ifdef SYS_close
  {SYS_close,"close","d"},
#endif
#ifdef SYS_read_nocancel
  {SYS_read_nocancel,"read","dpd"},
#endif
#ifdef SYS_write_nocancel
  {SYS_write_nocancel,"write","dBd"},
#endif
#ifdef SYS_open_nocancel
  {SYS_open_nocancel,"open","sOx"},
#endif
#ifdef SYS_close_nocancel
  {SYS_close_nocancel,"close","d"},
#endif
#ifdef SYS_openat
  {SYS_openat,"openat","dsOx"},
#endif
#ifdef SYS_access
  {SYS_access,"access","sA"},
#endif
#ifdef SYS_unlink
  {SYS_unlink,"unlink","s"},
#endif
#ifdef SYS_chdir
  {SYS_chdir,"chdir","s"},
#endif
#ifdef SYS_stat64
  {SYS_stat64,"stat64","sp"},
#endif
#ifdef SYS_lstat64
  {SYS_lstat64,"lstat64","sp"},
#endif
#ifdef SYS_fstat64
  {SYS_fstat64,"fstat64","dp"},
#endif
#ifdef SYS_execve
  {SYS_execve,"execve","spp"},
#endif
#ifdef SYS_lseek
  {SYS_lseek,"lseek","ddW"},
#endif
#ifdef SYS_ioctl
  {SYS_ioctl,"ioctl","dxp"},
#endif
#ifdef SYS_fcntl
  {SYS_fcntl,"fcntl","dxx"},
#endif
#ifdef SYS_kill
  {SYS_kill,"kill","dS"},
#endif
#ifdef SYS_dup
  {SYS_dup,"dup","d"},
#endif
#ifdef SYS_dup2
  {SYS_dup2,"dup2","dd"},
#endif
#ifdef SYS_getpid
  {SYS_getpid,"getpid",""},
#endif
#ifdef SYS_mmap
  {SYS_mmap,"mmap","pdPMdx"},
#endif
#ifdef SYS_munmap
  {SYS_munmap,"munmap","pd"},
#endif
#ifdef SYS_mprotect
  {SYS_mprotect,"mprotect","pdP"},
#endif
};

// Mach トラップ（x86:rax のクラス=1 / arm64:x16 が負）。番号→名前と引数シグネチャ。
// sig 文字: n=ポート名, r=ポート権利, m=メッセージ型disposition, v=VMフラグ, P=VM保護,
//           d=10進, x=16進, p=ポインタ, H=mach_msgヘッダ, G=mach_msgオプション。
struct MachTrap { int num; const char* name; const char* sig; };
const MachTrap kMachTraps[] = {
  {10,"_kernelrpc_mach_vm_allocate_trap","npdv"},
  {12,"_kernelrpc_mach_vm_deallocate_trap","nxd"},
  {14,"_kernelrpc_mach_vm_protect_trap","nxddP"},
  {15,"_kernelrpc_mach_vm_map_trap","npdxvP"},
  {16,"_kernelrpc_mach_port_allocate_trap","nrp"},
  {18,"_kernelrpc_mach_port_deallocate_trap","nn"},
  {19,"_kernelrpc_mach_port_mod_refs_trap","nnrd"},
  {20,"_kernelrpc_mach_port_move_member_trap","nnn"},
  {21,"_kernelrpc_mach_port_insert_right_trap","nnnm"},
  {22,"_kernelrpc_mach_port_insert_member_trap","nnn"},
  {23,"_kernelrpc_mach_port_extract_member_trap","nnn"},
  {24,"_kernelrpc_mach_port_construct_trap","npxp"},
  {25,"_kernelrpc_mach_port_destruct_trap","nndx"},
  {26,"mach_reply_port",""}, {27,"thread_self_trap",""}, {28,"task_self_trap",""},
  {29,"host_self_trap",""},
  {31,"mach_msg_trap","HGddxdx"}, {32,"mach_msg_overwrite_trap","HGddxdxp"},
  {33,"semaphore_signal_trap","n"}, {34,"semaphore_signal_all_trap","n"},
  {35,"semaphore_signal_thread_trap","nn"}, {36,"semaphore_wait_trap","n"},
  {37,"semaphore_wait_signal_trap","nn"}, {38,"semaphore_timedwait_trap","ndd"},
  {39,"semaphore_timedwait_signal_trap","nndd"},
  {41,"_kernelrpc_mach_port_guard_trap","nnxd"}, {42,"_kernelrpc_mach_port_unguard_trap","nnx"},
  {44,"task_name_for_pid","ndp"}, {45,"task_for_pid","ndp"}, {46,"pid_for_task","np"},
  {58,"pfz_exit",""}, {59,"swtch_pri","d"}, {60,"swtch",""},
  {61,"thread_switch","nxd"}, {62,"clock_sleep_trap","nxddp"},
  {89,"mach_continuous_time_trap",""}, {90,"mach_wait_until_trap","d"},
  {91,"mk_timer_create_trap",""}, {92,"mk_timer_destroy_trap","n"},
  {93,"mk_timer_arm_trap","nd"}, {94,"mk_timer_cancel_trap","np"},
  {100,"iokit_user_client_trap","nxxxxxxx"},
};

// 名前付きフラグを "|" で連結して "A|B (0x..)" に整形するヘルパ。
struct FlagBit { uint64_t bit; const char* name; };
static std::string joinFlags(uint64_t v, const FlagBit* bits, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) if (v & bits[i].bit) { if (!out.empty()) out += "|"; out += bits[i].name; }
  char hx[20]; snprintf(hx, sizeof(hx), "0x%llx", (unsigned long long)v);
  if (out.empty()) return hx;
  return out + " (" + hx + ")";
}

static std::string decOpenFlags(uint64_t v) {
  const char* acc = ((v & O_ACCMODE) == O_RDONLY) ? "O_RDONLY"
                  : ((v & O_ACCMODE) == O_WRONLY) ? "O_WRONLY"
                  : ((v & O_ACCMODE) == O_RDWR)   ? "O_RDWR" : "O_?";
  static const FlagBit B[] = {
    {O_CREAT,"O_CREAT"},{O_EXCL,"O_EXCL"},{O_TRUNC,"O_TRUNC"},{O_APPEND,"O_APPEND"},
    {O_NONBLOCK,"O_NONBLOCK"},{O_CLOEXEC,"O_CLOEXEC"},{O_NOFOLLOW,"O_NOFOLLOW"},{O_DIRECTORY,"O_DIRECTORY"},
  };
  std::string out = acc;
  for (auto& b : B) if (v & b.bit) out += std::string("|") + b.name;
  char hx[20]; snprintf(hx, sizeof(hx), " (0x%llx)", (unsigned long long)v);
  return out + hx;
}
static std::string decProt(uint64_t v) {
  if (v == 0) return "PROT_NONE (0x0)";
  static const FlagBit B[] = {{PROT_READ,"PROT_READ"},{PROT_WRITE,"PROT_WRITE"},{PROT_EXEC,"PROT_EXEC"}};
  return joinFlags(v, B, 3);
}
static std::string decMapFlags(uint64_t v) {
  static const FlagBit B[] = {
    {MAP_SHARED,"MAP_SHARED"},{MAP_PRIVATE,"MAP_PRIVATE"},{MAP_FIXED,"MAP_FIXED"},{MAP_ANON,"MAP_ANON"},
  };
  return joinFlags(v, B, 4);
}
static std::string decAccess(uint64_t v) {
  if (v == F_OK) return "F_OK (0x0)";
  static const FlagBit B[] = {{R_OK,"R_OK"},{W_OK,"W_OK"},{X_OK,"X_OK"}};
  return joinFlags(v, B, 3);
}
static std::string decWhence(uint64_t v) {
  const char* s = v == SEEK_SET ? "SEEK_SET" : v == SEEK_CUR ? "SEEK_CUR" : v == SEEK_END ? "SEEK_END" : nullptr;
  if (s) return s;
  char hx[20]; snprintf(hx, sizeof(hx), "0x%llx", (unsigned long long)v); return hx;
}
static std::string decSignal(uint64_t v) {
  const char* s = (v > 0 && v < (uint64_t)NSIG) ? sys_signame[v] : nullptr;
  char buf[24];
  if (s) { snprintf(buf, sizeof(buf), "SIG%s", s); for (char* p = buf + 3; *p; p++) *p = (char)toupper((unsigned char)*p); return buf; }
  snprintf(buf, sizeof(buf), "%lld", (long long)v); return buf;
}
static std::string decMachOption(uint64_t v) {
  static const FlagBit B[] = {
    {MACH_SEND_MSG,"MACH_SEND_MSG"}, {MACH_RCV_MSG,"MACH_RCV_MSG"},
    {MACH_RCV_LARGE,"MACH_RCV_LARGE"}, {MACH_RCV_LARGE_IDENTITY,"MACH_RCV_LARGE_IDENTITY"},
    {MACH_SEND_TIMEOUT,"MACH_SEND_TIMEOUT"}, {MACH_SEND_OVERRIDE,"MACH_SEND_OVERRIDE"},
    {MACH_SEND_INTERRUPT,"MACH_SEND_INTERRUPT"}, {MACH_SEND_NOTIFY,"MACH_SEND_NOTIFY"},
    {MACH_RCV_TIMEOUT,"MACH_RCV_TIMEOUT"}, {MACH_RCV_INTERRUPT,"MACH_RCV_INTERRUPT"},
#ifdef MACH_RCV_VOUCHER
    {MACH_RCV_VOUCHER,"MACH_RCV_VOUCHER"},
#endif
#ifdef MACH_RCV_SYNC_WAIT
    {MACH_RCV_SYNC_WAIT,"MACH_RCV_SYNC_WAIT"},
#endif
#ifdef MACH_SEND_SYNC_OVERRIDE
    {MACH_SEND_SYNC_OVERRIDE,"MACH_SEND_SYNC_OVERRIDE"},
#endif
    {MACH_SEND_TRAILER,"MACH_SEND_TRAILER"},
  };
  if (v == 0) return "MACH_MSG_OPTION_NONE (0x0)";
  return joinFlags(v, B, sizeof(B) / sizeof(B[0]));
}
static std::string decPortName(uint64_t v) {
  uint32_t n = (uint32_t)v;
  if (n == 0) return "MACH_PORT_NULL";
  if (n == (uint32_t)~0u) return "MACH_PORT_DEAD";
  char b[16]; snprintf(b, sizeof(b), "0x%x", n); return b;
}
static std::string decPortRight(uint64_t v) {
  switch (v) {
    case MACH_PORT_RIGHT_SEND: return "MACH_PORT_RIGHT_SEND";
    case MACH_PORT_RIGHT_RECEIVE: return "MACH_PORT_RIGHT_RECEIVE";
    case MACH_PORT_RIGHT_SEND_ONCE: return "MACH_PORT_RIGHT_SEND_ONCE";
    case MACH_PORT_RIGHT_PORT_SET: return "MACH_PORT_RIGHT_PORT_SET";
    case MACH_PORT_RIGHT_DEAD_NAME: return "MACH_PORT_RIGHT_DEAD_NAME";
    default: { char b[24]; snprintf(b, sizeof(b), "right(%lld)", (long long)v); return b; }
  }
}
static std::string decMsgType(uint64_t v) { // disposition
  switch (v) {
    case MACH_MSG_TYPE_MOVE_RECEIVE: return "MOVE_RECEIVE";
    case MACH_MSG_TYPE_MOVE_SEND: return "MOVE_SEND";
    case MACH_MSG_TYPE_MOVE_SEND_ONCE: return "MOVE_SEND_ONCE";
    case MACH_MSG_TYPE_COPY_SEND: return "COPY_SEND";
    case MACH_MSG_TYPE_MAKE_SEND: return "MAKE_SEND";
    case MACH_MSG_TYPE_MAKE_SEND_ONCE: return "MAKE_SEND_ONCE";
    case MACH_MSG_TYPE_COPY_RECEIVE: return "COPY_RECEIVE";
    case 0: return "0";
    default: { char b[16]; snprintf(b, sizeof(b), "%lld", (long long)v); return b; }
  }
}
static std::string decVmFlags(uint64_t v) {
  static const FlagBit B[] = {
    {VM_FLAGS_ANYWHERE,"VM_FLAGS_ANYWHERE"}, {VM_FLAGS_PURGABLE,"VM_FLAGS_PURGABLE"},
    {VM_FLAGS_FIXED | 0,"" }, // placeholder（FIXED=0 は後で特別扱い）
#ifdef VM_FLAGS_OVERWRITE
    {VM_FLAGS_OVERWRITE,"VM_FLAGS_OVERWRITE"},
#endif
#ifdef VM_FLAGS_RANDOM_ADDR
    {VM_FLAGS_RANDOM_ADDR,"VM_FLAGS_RANDOM_ADDR"},
#endif
  };
  if ((v & VM_FLAGS_ANYWHERE) == 0) { // ANYWHERE 無し = FIXED
    if (v == 0) return "VM_FLAGS_FIXED (0x0)";
  }
  std::string out;
  for (auto& b : B) if (b.name[0] && (v & b.bit)) { if (!out.empty()) out += "|"; out += b.name; }
  char hx[20]; snprintf(hx, sizeof(hx), "0x%llx", (unsigned long long)v);
  return out.empty() ? std::string(hx) : out + " (" + hx + ")";
}
// mach_msg の msgh_bits を分解（remote/local の disposition と COMPLEX）。
static std::string decMsghBits(uint32_t bits) {
  std::string s = "remote=" + decMsgType(MACH_MSGH_BITS_REMOTE(bits));
  uint32_t loc = MACH_MSGH_BITS_LOCAL(bits);
  if (loc) s += " local=" + decMsgType(loc);
  if (bits & MACH_MSGH_BITS_COMPLEX) s += " COMPLEX";
  return s;
}
static const char* machDescType(uint32_t t) {
  switch (t) {
    case MACH_MSG_PORT_DESCRIPTOR: return "port";
    case MACH_MSG_OOL_DESCRIPTOR: return "ool";
    case MACH_MSG_OOL_PORTS_DESCRIPTOR: return "ool_ports";
#ifdef MACH_MSG_OOL_VOLATILE_DESCRIPTOR
    case MACH_MSG_OOL_VOLATILE_DESCRIPTOR: return "ool_volatile";
#endif
#ifdef MACH_MSG_GUARDED_PORT_DESCRIPTOR
    case MACH_MSG_GUARDED_PORT_DESCRIPTOR: return "guarded_port";
#endif
    default: return "?";
  }
}
} // namespace

std::string Session::syscallAtStop() {
  if (!stopped_.load()) return std::string();
  Arch& a = target_.arch();

  // 停止位置の命令が syscall/svc か（ブレイク命令バイトは元に戻して判定）
  uint8_t code[16] = {}; size_t got = 0;
  if (!target_.readMemory(stopPc_, code, sizeof(code), &got) || got == 0) return std::string();
  bps_.maskInto(stopPc_, code, got);
  Disassembler dis(a);
  auto insns = dis.disasm(code, got, stopPc_, 1);
  if (insns.empty() || insns[0].kind != InsnKind::Syscall) return std::string();

  uint64_t raw = a.syscallNumberReg(stopState_.data());
  // クラス判定: x86 は rax 上位バイト（Mach=1, Unix=2）。arm64 は x16 が負なら Mach トラップ。
  int cls = (int)((raw >> 24) & 0xff);
  bool mach; uint64_t num;
  if (cls == 2)                   { mach = false; num = raw & 0xffffff; } // x86 Unix
  else if (cls == 1)              { mach = true;  num = raw & 0xffffff; } // x86 Mach
  else if ((int64_t)raw < 0)      { mach = true;  num = (uint64_t)(-(int64_t)raw); } // arm64 Mach
  else                            { mach = false; num = raw & 0xffffff; } // arm64 Unix

  // 参照先ヘルパ
  auto cstr = [&](uint64_t p) -> std::string {
    if (!p) return "NULL";
    char buf[128] = {}; size_t n = 0;
    if (!target_.readMemory(p, buf, sizeof(buf) - 1, &n) && n == 0) return "";
    std::string out = "\"";
    for (size_t i = 0; i < sizeof(buf) - 1 && buf[i]; i++) {
      unsigned char c = (unsigned char)buf[i];
      if (c == '\n') out += "\\n"; else if (c == '\t') out += "\\t";
      else if (isprint(c)) out += (char)c; else { char h[5]; snprintf(h, sizeof(h), "\\x%02x", c); out += h; }
      if (out.size() > 80) { out += "…"; break; }
    }
    return out + "\"";
  };
  auto buffer = [&](uint64_t p, uint64_t len) -> std::string {
    if (!p) return "NULL";
    size_t want = (size_t)(len < 48 ? len : 48);
    std::vector<uint8_t> b(want ? want : 1); size_t n = 0;
    target_.readMemory(p, b.data(), want, &n);
    std::string out = "\"";
    for (size_t i = 0; i < n; i++) {
      unsigned char c = b[i];
      if (c == '\n') out += "\\n"; else if (c == '\t') out += "\\t";
      else if (isprint(c)) out += (char)c; else { char h[5]; snprintf(h, sizeof(h), "\\x%02x", c); out += h; }
    }
    out += "\"";
    if (len > want) out += "…";
    return out;
  };
  // mach_msg_header_t を読む: msgh_bits 分解・id・ポート、COMPLEX ならディスクリプタも解析。
  auto machHeader = [&](uint64_t p) -> std::string {
    char hx[24]; snprintf(hx, sizeof(hx), "0x%llx", (unsigned long long)p);
    if (!p) return "NULL";
    uint32_t h[6] = {}; // bits,size,remote,local,voucher,id
    if (!target_.readMemory(p, h, sizeof(h))) return hx;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s {id=%d size=%u %s rport=%s lport=%s}", hx, (int)h[5], h[1],
             decMsghBits(h[0]).c_str(), decPortName(h[2]).c_str(), decPortName(h[3]).c_str());
    std::string out = buf;

    // COMPLEX: ヘッダ(24B)直後の body(descriptor_count) とディスクリプタ列を解析（64bit ユーザ ABI）。
    if (h[0] & MACH_MSGH_BITS_COMPLEX) {
      uint32_t dcount = 0;
      if (target_.readMemory(p + 24, &dcount, 4) && dcount && dcount < 256) {
        out += "  descriptors=" + std::to_string(dcount) + " [";
        uint64_t d = p + 28; // body の直後
        for (uint32_t i = 0; i < dcount && i < 8; i++) {
          uint8_t raw[16] = {};
          if (!target_.readMemory(d, raw, sizeof(raw))) break;
          uint32_t w8; memcpy(&w8, raw + 8, 4);         // offset 8 の 32bit（type は最上位バイト）
          uint32_t type = (w8 >> 24) & 0xff;
          if (i) out += ", ";
          out += machDescType(type);
          if (type == MACH_MSG_PORT_DESCRIPTOR) {        // 12B: name(0), …, disp(byte10), type(byte11)
            uint32_t name; memcpy(&name, raw, 4);
            uint32_t disp = (w8 >> 16) & 0xff;
            out += "(" + decPortName(name) + "," + decMsgType(disp) + ")";
            d += 12;
          } else if (type == MACH_MSG_OOL_DESCRIPTOR) {  // 16B: addr(0..8), type@8, size@12
            uint32_t sz; memcpy(&sz, raw + 12, 4);
            out += "(size=" + std::to_string(sz) + ")";
            d += 16;
          } else if (type == MACH_MSG_OOL_PORTS_DESCRIPTOR) { // 16B: addr, disp@byte10, type@11, count@12
            uint32_t cnt; memcpy(&cnt, raw + 12, 4);
            uint32_t disp = (w8 >> 16) & 0xff;
            out += "(" + decMsgType(disp) + ",count=" + std::to_string(cnt) + ")";
            d += 16;
          } else { d += 16; } // 未知は 16B 進めて継続（best-effort）
        }
        if (dcount > 8) out += ", …";
        out += "]";
      }
    }
    return out;
  };

  // シグネチャ 1 文字に従って引数 i を整形（Mach/BSD 共通）
  auto fmtArg = [&](const char* sig, int i) -> std::string {
    uint64_t v = a.syscallArgReg(stopState_.data(), i);
    char hex[24];
    switch (sig[i]) {
      case 'd': return std::to_string((long long)(int64_t)v);
      case 's': snprintf(hex, sizeof(hex), "0x%llx ", (unsigned long long)v); return std::string(hex) + cstr(v);
      case 'B': { uint64_t len = a.syscallArgReg(stopState_.data(), i + 1);
                  snprintf(hex, sizeof(hex), "0x%llx ", (unsigned long long)v); return std::string(hex) + buffer(v, len); }
      case 'O': return decOpenFlags(v);
      case 'P': return decProt(v);
      case 'M': return decMapFlags(v);
      case 'A': return decAccess(v);
      case 'W': return decWhence(v);
      case 'S': return decSignal(v);
      case 'G': return decMachOption(v);          // mach_msg オプション
      case 'H': return machHeader(v);             // mach_msg_header_t*
      case 'n': return decPortName(v);            // Mach ポート名
      case 'r': return decPortRight(v);           // Mach ポート権利
      case 'm': return decMsgType(v);             // メッセージ型 disposition
      case 'v': return decVmFlags(v);             // VM フラグ
      default:  snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)v); return hex; // x / p
    }
  };
  auto formatArgs = [&](const char* sig) -> std::string {
    std::string args; int n = (int)strlen(sig);
    for (int i = 0; i < n && i < 6; i++) { if (i) args += ", "; args += fmtArg(sig, i); }
    return args;
  };

  if (mach) { // Mach トラップ
    const MachTrap* mt = nullptr;
    for (const auto& m : kMachTraps) if ((uint64_t)m.num == num) { mt = &m; break; }
    if (mt) return std::string(mt->name) + "(" + formatArgs(mt->sig) + ")";
    return "mach_trap#" + std::to_string((long long)num) + "(" + formatArgs("xxx") + ")";
  }

  const Scall* info = nullptr;
  for (const auto& s : kScalls) if ((uint64_t)s.num == num) { info = &s; break; }
  std::string name = info ? info->name : (std::string("syscall#") + std::to_string((long long)num));
  return name + "(" + formatArgs(info ? info->sig : "xxx") + ")";
}

void Session::detach() {
  stopSymbolWarming(); // target 破棄前に warming スレッドを必ず止める
  if (!target_.isAttached()) return;
  stopExitMonitor();
  if (stopped_ && manualStop_) {
    // Ctrl+C 停止中: タスクは suspend 済み。ここで後片付けして再開する。
    bps_.removeAll();
    writeHwToThreads(); // デバッグレジスタも全消去
    setStep(stopThread_, false);
    singleStepping_ = false; rearmValid_ = false; tempValid_ = false; hwRearmIds_.clear(); watchStepId_ = -1;
    target_.resume();
    stopped_ = false; manualStop_ = false;
  } else if (stopped_) {
    // 例外スレッドにクリーンな再開を任せ、完了を待つ
    decide(Resume::Detach);
    std::unique_lock<std::mutex> lk(m_);
    ackCv_.wait(lk, [this] { return detachAck_; });
    detachAck_ = false;
  } else {
    // 実行中: こちらで一瞬止めてブレイクを全部外す
    target_.suspend();
    bps_.removeAll();
    writeHwToThreads(); // デバッグレジスタも全消去
    target_.resume();
  }
  es_.uninstall();
  target_.detach();
}

void Session::decide(Resume r) {
  { std::lock_guard<std::mutex> lk(m_); decision_ = r; }
  resumeCv_.notify_one();
}

// 全スレッドのデバッグレジスタへ現在の HW ブレイク集合を反映。呼び出し側がタスク停止を保証する。
void Session::writeHwToThreads() {
  if (!target_.isAttached()) return;
  std::vector<uint8_t> ds(arch().debugStateCount() * sizeof(natural_t), 0);
  for (auto t : target_.threads()) {
    if (!target_.getDebugState(t, ds.data())) continue; // 既存の SS 等を保つため read-modify-write
    bps_.applyHwInto(ds.data());
    target_.setDebugState(t, ds.data());
  }
}

void Session::syncHwBreaks() {
  if (!target_.isAttached()) return;
  bool needSuspend = !stopped_.load(); // 停止中はタスク凍結済み。実行中なら一瞬止める。
  if (needSuspend) target_.suspend();
  writeHwToThreads();
  if (needSuspend) target_.resume();
}

void Session::clearHwHitOn(thread_act_t t) {
  std::vector<uint8_t> ds(arch().debugStateCount() * sizeof(natural_t), 0);
  if (!target_.getDebugState(t, ds.data())) return;
  arch().clearHwHitStatus(ds.data());
  target_.setDebugState(t, ds.data());
}

// ウォッチ命中の判定。x86 は DR6 の発火ビットからスロットを得てウォッチを特定し、DR6 をクリアする。
// arm64 は debug state からは特定できないので、有効なウォッチの先頭を暫定採用する（どれが鳴ったかは不明）。
const Breakpoint* Session::detectWatchHit(thread_act_t thread) {
  const Breakpoint* anyEnabledWatch = nullptr;
  for (const auto& b : bps_.list()) if (b.hw && b.watch && b.enabled) { anyEnabledWatch = &b; break; }
  if (!anyEnabledWatch) return nullptr; // そもそもウォッチが無ければウォッチ命中ではない

  std::vector<uint8_t> ds(arch().debugStateCount() * sizeof(natural_t), 0);
  if (!target_.getDebugState(thread, ds.data())) return anyEnabledWatch;
  int slot = arch().hwBreakHit(ds.data());
  arch().clearHwHitStatus(ds.data());
  target_.setDebugState(thread, ds.data()); // 発火ビットを消して戻す

  if (slot >= 0)
    for (const auto& b : bps_.list())
      if (b.hw && b.watch && b.enabled && b.slot == slot) return &b;
  return anyEnabledWatch; // スロット不明（arm64）: 暫定
}

void Session::interrupt() {
  if (!target_.isAttached()) return;
  std::lock_guard<std::mutex> lk(m_);
  if (stopped_.load()) return; // 既に停止中（例外や別の割り込み）
  Arch& a = target_.arch();
  target_.suspend(); // タスク全体を凍結
  auto threads = target_.threads();
  if (threads.empty()) { target_.resume(); return; }
  thread_act_t t = threads[0];
  target_.getThreadState(t, stopState_.data());
  stopPc_ = a.pc(stopState_.data());
  stopThread_ = t;
  manualStop_ = true;
  lastWatchId_ = -1;
  stopExc_ = 0;
  stopped_ = true;
  queue_.push({Event::Stopped, 0});
}

// Ctrl+C 停止からの再開（例外スレッドは待機していないのでこちらで applyResume する）
void Session::resumeManual(Resume r) {
  stopped_ = false;
  manualStop_ = false;
  applyResume(r, stopThread_);
}

void Session::cont()     { if (manualStop_) resumeManual(Resume::Cont);     else decide(Resume::Cont); }
void Session::stepInto() { if (manualStop_) resumeManual(Resume::Step);     else decide(Resume::Step); }
void Session::stepOver() { if (manualStop_) resumeManual(Resume::StepOver); else decide(Resume::StepOver); }

bool Session::setRegister(const std::string& name, uint64_t value) {
  if (!stopped_.load()) return false;
  Arch& a = target_.arch();
  if (!a.writeRegisterByName(stopState_.data(), name, value)) return false;
  if (!target_.setThreadState(stopThread_, stopState_.data())) return false;
  stopPc_ = a.pc(stopState_.data()); // pc を書き換えた場合に表示用も合わせる
  return true;
}

void Session::setStep(thread_act_t t, bool on) {
  Arch& a = target_.arch();
  if (a.singleStepUsesDebugState()) {
    std::vector<uint8_t> ds(a.debugStateCount() * sizeof(natural_t), 0);
    target_.getDebugState(t, ds.data());
    a.setSingleStep(ds.data(), on);
    target_.setDebugState(t, ds.data());
  } else {
    std::vector<uint8_t> ts(a.threadStateCount() * sizeof(natural_t), 0);
    target_.getThreadState(t, ts.data());
    a.setSingleStep(ts.data(), on);
    target_.setThreadState(t, ts.data());
  }
}

kern_return_t Session::onException(thread_act_t thread, exception_type_t exc,
                                   const int64_t* codes, int n) {
  Arch& a = target_.arch();
  target_.suspend(); // 判断・停止中はタスク全体を凍結

  std::vector<uint8_t> ts(a.threadStateCount() * sizeof(natural_t), 0);
  target_.getThreadState(thread, ts.data());
  uint64_t pc = a.pc(ts.data());

  // ブレイク由来でない例外（不正アドレス参照・不正命令・算術例外など）＝クラッシュ。
  // ブレイク／ステップの張り直しロジックは通さず、停止理由を添えてそのまま止める。
  // PC はフォルトした命令そのものを指す（巻き戻し不要）。
  if (exc != EXC_BREAKPOINT) {
    stopExc_ = exc;
    stopCode0_ = (n > 0) ? codes[0] : 0;
    stopCode1_ = (n > 1) ? codes[1] : 0;
    lastWatchId_ = -1;
    // またぎステップ中に別スレッドがクラッシュした等の保留があれば戻しておく
    if (rearmValid_) { bps_.addSoft(rearmAddr_); rearmValid_ = false; }
    if (!hwRearmIds_.empty()) {
      for (int id : hwRearmIds_) bps_.hwTransientEnable(id);
      hwRearmIds_.clear();
      writeHwToThreads();
    }
    if (singleStepping_) { singleStepping_ = false; setStep(thread, false); }
    stopPc_ = pc;

    target_.getThreadState(thread, stopState_.data());
    stopThread_ = thread;
    manualStop_ = false; // 例外スレッドが resume 決定を待つ
    stopped_ = true;
    queue_.push({Event::Stopped, 0});

    Resume d;
    {
      std::unique_lock<std::mutex> lk(m_);
      resumeCv_.wait(lk, [this] { return decision_ != Resume::None; });
      d = decision_;
      decision_ = Resume::None;
    }
    stopped_ = false;
    applyResume(d, thread);
    return KERN_SUCCESS;
  }

  stopExc_ = 0; // 通常のブレイク/ステップ停止
  bool wasStepping = singleStepping_;
  lastWatchId_ = -1; // 既定。ウォッチ命中時のみ下で設定

  // 直前に外したソフトブレイクを張り直す（1 命令進んだ後）
  if (rearmValid_) { bps_.addSoft(rearmAddr_); rearmValid_ = false; }
  // またぎステップ中だけ外していた HW ブレイク/ウォッチを戻す（タスクは suspend 済み）
  if (!hwRearmIds_.empty()) {
    for (int id : hwRearmIds_) bps_.hwTransientEnable(id);
    hwRearmIds_.clear();
    writeHwToThreads();
  }

  if (wasStepping) {
    singleStepping_ = false;
    setStep(thread, false); // TF/SS を落とす
    if (!stopAfterStep_) {
      // cont で bp をまたいだだけ → ユーザ停止せず走行継続
      target_.resume();
      return KERN_SUCCESS;
    }
    stopPc_ = pc; // step が着地 → ここで停止
  } else {
    // 新規のソフトブレイク #BP。PC を命令先頭へ戻す。
    uint64_t at = pc - a.softBreakPcBackup();
    const Breakpoint* hwb = bps_.userAt(pc); // HW は PC が命令そのもの（巻き戻し不要）
    if (bps_.has(at) || (tempValid_ && at == tempAddr_)) {
      a.setPc(ts.data(), at);
      target_.setThreadState(thread, ts.data());
      stopPc_ = at;
      if (tempValid_ && at == tempAddr_) { bps_.removeSoft(tempAddr_); tempValid_ = false; }
      else bps_.onHit(at); // ユーザブレイク命中（一時ブレイクは数えない）
    } else if (hwb && hwb->hw && !hwb->watch && hwb->enabled) {
      stopPc_ = pc;        // HW 実行ブレイク命中
      bps_.onHit(pc);
      clearHwHitOn(thread); // DR6 の発火ビットを消す（次のウォッチ判定が古いビットに惑わされないよう）
    } else if (const Breakpoint* w = detectWatchHit(thread)) {
      stopPc_ = pc;        // ウォッチポイント命中（PC はアクセス命令、または直後）
      bps_.onHit(w->addr);
      lastWatchId_ = w->id;
      // arm64 はアクセス命令で止まり再実行するので、再開時に 1 命令だけ跨ぐ必要がある。
      if (a.hwWatchStopsBeforeAccess()) watchStepId_ = w->id;
    } else {
      stopPc_ = pc; // 未知のトラップでも一応止める
    }
  }

  // ユーザに見せる停止
  target_.getThreadState(thread, stopState_.data()); // PC 補正後の状態
  stopThread_ = thread;
  manualStop_ = false; // これは例外による停止（例外スレッドが待機する）
  stopped_ = true;
  queue_.push({Event::Stopped, 0});

  // resume 決定を待つ
  Resume d;
  {
    std::unique_lock<std::mutex> lk(m_);
    resumeCv_.wait(lk, [this] { return decision_ != Resume::None; });
    d = decision_;
    decision_ = Resume::None;
  }
  stopped_ = false;

  applyResume(d, thread);
  return KERN_SUCCESS;
}

void Session::applyResume(Resume d, thread_act_t thread) {
  Arch& a = target_.arch();

  // クラッシュ停止からの継続/ステップ。未解決のフォルト命令は「またげ」ない
  // （KERN_SUCCESS で再実行しても同じ所でまた落ちる）ので、例外ポートを本来のハンドラへ
  // 戻してから再実行させ、対象を通常どおりクラッシュ終了させる（＝シグナルを通す）。
  // Detach はこの後の共通処理（全ブレイク除去→resume）でクリーンに手放す。
  if (stopExc_ != 0 && d != Resume::Detach) {
    stopExc_ = 0;
    es_.restoreOriginalPorts(); // 以後この対象の例外はこちらへ来ない
    target_.resume();
    return;
  }
  stopExc_ = 0;

  // 現在 PC に張ってあるユーザブレイクを「またぐ」ための一時解除。
  //   SW: INT3 をいったん外し、1 命令後に張り直す（rearmValid_）。
  //   HW: デバッグレジスタ反映を一瞬外し、1 命令後に戻す（hwRearmIds_）。
  // どちらも「外す → 単一ステップ → 戻す」の同じ所作で、x86/arm64 共通に扱える。
  auto uncoverBreakAtPc = [&]() -> bool {
    bool covered = false;
    bool hwTouched = false;
    if (bps_.has(stopPc_)) {
      bps_.removeSoft(stopPc_);
      rearmValid_ = true; rearmAddr_ = stopPc_;
      covered = true;
    }
    const Breakpoint* ub = bps_.userAt(stopPc_);
    if (ub && ub->hw && !ub->watch && ub->enabled) { // 現在 PC の HW 実行ブレイク
      bps_.hwTransientDisable(ub->id);
      hwRearmIds_.insert(ub->id);
      hwTouched = true; covered = true;
    }
    if (watchStepId_ >= 0) { // arm64: 直前のウォッチ命中を 1 命令跨ぐ
      bps_.hwTransientDisable(watchStepId_);
      hwRearmIds_.insert(watchStepId_);
      watchStepId_ = -1;
      hwTouched = true; covered = true;
    }
    if (hwTouched) writeHwToThreads(); // タスクは suspend 済み
    return covered;
  };

  auto stepOverBreakThenRun = [&](bool stopAfter) {
    bool covered = uncoverBreakAtPc();
    if (covered || stopAfter) {
      singleStepping_ = true; stopAfterStep_ = stopAfter;
      setStep(thread, true);
    } else {
      setStep(thread, false);
    }
    target_.resume();
  };

  switch (d) {
    case Resume::Cont:
      stepOverBreakThenRun(false);
      break;

    case Resume::Step:
      // bp 上なら外して鳴らさず 1 命令、着地で停止
      uncoverBreakAtPc();
      singleStepping_ = true; stopAfterStep_ = true;
      setStep(thread, true);
      target_.resume();
      break;

    case Resume::StepOver: {
      // 現在命令を 1 つ逆アセンブルして call なら戻り先に一時 bp を置いて継続
      uint8_t code[16] = {};
      size_t got = 0;
      target_.readMemory(stopPc_, code, sizeof(code), &got);
      Disassembler dis(a);
      auto insns = dis.disasm(code, got, stopPc_, 1);
      if (!insns.empty() && insns[0].kind == InsnKind::Call) {
        tempAddr_ = stopPc_ + insns[0].size;
        tempValid_ = true;
        bps_.addSoft(tempAddr_);
        stepOverBreakThenRun(false); // 継続（戻り先 bp で止まる）
      } else {
        uncoverBreakAtPc();
        singleStepping_ = true; stopAfterStep_ = true;
        setStep(thread, true);
        target_.resume();
      }
      break;
    }

    case Resume::Detach:
      bps_.removeAll();
      writeHwToThreads(); // デバッグレジスタも全消去
      setStep(thread, false);
      singleStepping_ = false; rearmValid_ = false; tempValid_ = false; hwRearmIds_.clear(); watchStepId_ = -1;
      target_.resume();
      { std::lock_guard<std::mutex> lk(m_); detachAck_ = true; }
      ackCv_.notify_one();
      break;

    case Resume::None:
      target_.resume();
      break;
  }
}

} // namespace ta2
