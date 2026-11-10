#include "core/ProcessList.h"

#include <algorithm>
#include <cstring>

#include <libproc.h>
#include <mach/mach.h>
#include <sys/sysctl.h>

namespace ta2 {

namespace {
// 実際にアタッチせずに可否だけ確かめる。task_for_pid で送信権を得られれば可。
// 得た権利はすぐ解放する（対象を止めたりはしない。純粋な可否チェック）。
bool probeAttachable(pid_t pid) {
  mach_port_t task = MACH_PORT_NULL;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
  if (kr != KERN_SUCCESS) return false;
  mach_port_deallocate(mach_task_self(), task);
  return true;
}

// KERN_PROCARGS2 の形式: [int argc][実行パス\0][詰めの\0...][argv0\0 argv1\0 ...][env...]
// argv[1..] を空白で連結して返す（引数が無ければ空）。root でなければ読めないことがある。
std::string procArgs(pid_t pid, std::vector<char>& buf) {
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  size_t sz = buf.size();
  if (sysctl(mib, 3, buf.data(), &sz, nullptr, 0) != 0) return std::string();
  if (sz < sizeof(int)) return std::string();

  int argc = 0;
  std::memcpy(&argc, buf.data(), sizeof(int));
  const char* p = buf.data() + sizeof(int);
  const char* end = buf.data() + sz;

  while (p < end && *p != '\0') p++;   // 実行パスを飛ばす
  while (p < end && *p == '\0') p++;   // 詰めの NUL を飛ばす

  std::string out;
  for (int i = 0; i < argc && p < end; i++) {
    const char* start = p;
    while (p < end && *p != '\0') p++;
    if (i >= 1) {                      // argv[0] は名前と重複しがちなので外す
      if (!out.empty()) out += ' ';
      out.append(start, (size_t)(p - start));
    }
    if (p < end) p++;                  // NUL を飛ばす
  }
  return out;
}
} // namespace

std::vector<ProcessInfo> listProcesses() {
  std::vector<ProcessInfo> out;

  int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
  if (bytes <= 0) return out;
  // 取得と取得の間にプロセスが増える余地を少し持たせる。
  int cap = bytes / (int)sizeof(pid_t) + 32;
  std::vector<pid_t> pids(cap, 0);
  bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), cap * (int)sizeof(pid_t));
  if (bytes <= 0) return out;
  int count = bytes / (int)sizeof(pid_t);

  // KERN_PROCARGS2 用のバッファは argmax 一回分を使い回す。
  int argmax = 0;
  {
    int mib[2] = {CTL_KERN, KERN_ARGMAX};
    size_t sz = sizeof(argmax);
    if (sysctl(mib, 2, &argmax, &sz, nullptr, 0) != 0 || argmax <= 0) argmax = 256 * 1024;
  }
  std::vector<char> argbuf(argmax);

  out.reserve(count);
  for (int i = 0; i < count; i++) {
    pid_t pid = pids[i];
    if (pid <= 0) continue;

    ProcessInfo info;
    info.pid = pid;

    char pathbuf[PROC_PIDPATHINFO_MAXSIZE] = {};
    int r = proc_pidpath(pid, pathbuf, sizeof(pathbuf));
    if (r > 0) {
      info.path.assign(pathbuf, (size_t)r);
      size_t slash = info.path.rfind('/');
      info.name = (slash == std::string::npos) ? info.path : info.path.substr(slash + 1);
    } else {
      // パスが取れない（権限など）場合はプロセス名だけでも。
      char namebuf[2 * MAXCOMLEN + 1] = {};
      if (proc_name(pid, namebuf, sizeof(namebuf)) > 0) info.name = namebuf;
    }

    info.attachable = probeAttachable(pid);
    info.args = procArgs(pid, argbuf);

    out.push_back(std::move(info));
  }

  std::sort(out.begin(), out.end(),
            [](const ProcessInfo& a, const ProcessInfo& b) { return a.pid < b.pid; });
  return out;
}

} // namespace ta2
