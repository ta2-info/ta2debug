#include "core/SyscallTables.h"

// v1 src/core/Session.cpp の無名名前空間（kScalls / kScallNames / kMachTraps / dec* / joinFlags）を
// そのまま移したもの。表の中身・デコード規則は変えていない（設計書 §4.2）。
// 表は言語非依存データ（syscall 名・フラグ名は翻訳対象外。§8.1-4）。

#include <cctype>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <mach/port.h>
#include <mach/vm_prot.h>
#include <mach/vm_statistics.h>

namespace ta2 {
namespace syscall_tables {

namespace {
// システムコール表。sig は引数ごとの型: d=10進, x=16進, p=ポインタ, s=C文字列(参照先), B=バッファ(長さは次の引数)。
const Entry kScalls[] = {
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

// 番号→名前だけの網羅表（SDK の全 SYS_*）。kScalls に無い syscall の素の名前表示に使う。
// SDK <sys/syscall.h> から自動生成（SyscallNames.inc）。sig が無いので引数は汎用（xxx）で出す。
struct ScallName { int num; const char* name; };
const ScallName kScallNames[] = {
#include "SyscallNames.inc"
};

// Mach トラップ（x86:rax のクラス=1 / arm64:x16 が負）。番号→名前と引数シグネチャ。
// sig 文字: n=ポート名, r=ポート権利, m=メッセージ型disposition, v=VMフラグ, P=VM保護,
//           d=10進, x=16進, p=ポインタ, H=mach_msgヘッダ, G=mach_msgオプション。
const Entry kMachTraps[] = {
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
} // namespace

const Entry* findBsd(uint64_t num) {
  for (const auto& s : kScalls) if ((uint64_t)s.num == num) return &s;
  return nullptr;
}

const char* bsdName(uint64_t num) {
  for (const auto& s : kScallNames) if ((uint64_t)s.num == num) return s.name;
  return nullptr;
}

const Entry* findMachTrap(uint64_t num) {
  for (const auto& m : kMachTraps) if ((uint64_t)m.num == num) return &m;
  return nullptr;
}

namespace {
struct FlagBit { uint64_t bit; const char* name; };
static std::string joinFlags(uint64_t v, const FlagBit* bits, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) if (v & bits[i].bit) { if (!out.empty()) out += "|"; out += bits[i].name; }
  char hx[20]; snprintf(hx, sizeof(hx), "0x%llx", (unsigned long long)v);
  if (out.empty()) return hx;
  return out + " (" + hx + ")";
}

} // namespace

std::string decOpenFlags(uint64_t v) {
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
std::string decProt(uint64_t v) {
  if (v == 0) return "PROT_NONE (0x0)";
  static const FlagBit B[] = {{PROT_READ,"PROT_READ"},{PROT_WRITE,"PROT_WRITE"},{PROT_EXEC,"PROT_EXEC"}};
  return joinFlags(v, B, 3);
}
std::string decMapFlags(uint64_t v) {
  static const FlagBit B[] = {
    {MAP_SHARED,"MAP_SHARED"},{MAP_PRIVATE,"MAP_PRIVATE"},{MAP_FIXED,"MAP_FIXED"},{MAP_ANON,"MAP_ANON"},
  };
  return joinFlags(v, B, 4);
}
std::string decAccess(uint64_t v) {
  if (v == F_OK) return "F_OK (0x0)";
  static const FlagBit B[] = {{R_OK,"R_OK"},{W_OK,"W_OK"},{X_OK,"X_OK"}};
  return joinFlags(v, B, 3);
}
std::string decWhence(uint64_t v) {
  const char* s = v == SEEK_SET ? "SEEK_SET" : v == SEEK_CUR ? "SEEK_CUR" : v == SEEK_END ? "SEEK_END" : nullptr;
  if (s) return s;
  char hx[20]; snprintf(hx, sizeof(hx), "0x%llx", (unsigned long long)v); return hx;
}
std::string decSignal(uint64_t v) {
  const char* s = (v > 0 && v < (uint64_t)NSIG) ? sys_signame[v] : nullptr;
  char buf[24];
  if (s) { snprintf(buf, sizeof(buf), "SIG%s", s); for (char* p = buf + 3; *p; p++) *p = (char)toupper((unsigned char)*p); return buf; }
  snprintf(buf, sizeof(buf), "%lld", (long long)v); return buf;
}
std::string decMachOption(uint64_t v) {
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
std::string decPortName(uint64_t v) {
  uint32_t n = (uint32_t)v;
  if (n == 0) return "MACH_PORT_NULL";
  if (n == (uint32_t)~0u) return "MACH_PORT_DEAD";
  char b[16]; snprintf(b, sizeof(b), "0x%x", n); return b;
}
std::string decPortRight(uint64_t v) {
  switch (v) {
    case MACH_PORT_RIGHT_SEND: return "MACH_PORT_RIGHT_SEND";
    case MACH_PORT_RIGHT_RECEIVE: return "MACH_PORT_RIGHT_RECEIVE";
    case MACH_PORT_RIGHT_SEND_ONCE: return "MACH_PORT_RIGHT_SEND_ONCE";
    case MACH_PORT_RIGHT_PORT_SET: return "MACH_PORT_RIGHT_PORT_SET";
    case MACH_PORT_RIGHT_DEAD_NAME: return "MACH_PORT_RIGHT_DEAD_NAME";
    default: { char b[24]; snprintf(b, sizeof(b), "right(%lld)", (long long)v); return b; }
  }
}
std::string decMsgType(uint64_t v) { // disposition
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
std::string decVmFlags(uint64_t v) {
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
std::string decMsghBits(uint32_t bits) {
  std::string s = "remote=" + decMsgType(MACH_MSGH_BITS_REMOTE(bits));
  uint32_t loc = MACH_MSGH_BITS_LOCAL(bits);
  if (loc) s += " local=" + decMsgType(loc);
  if (bits & MACH_MSGH_BITS_COMPLEX) s += " COMPLEX";
  return s;
}
const char* machDescType(uint32_t t) {
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

} // namespace syscall_tables
} // namespace ta2
