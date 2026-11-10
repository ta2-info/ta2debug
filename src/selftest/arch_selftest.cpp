// Arch 抽象のセルフテスト。ホストアーキ（この環境では x86_64）の実装を動かして、
// レジスタ・ソフトブレイク・単一ステップ・式用の名前引きが筋どおりかを確認する。
// arm64 実装は -arch arm64 のクロスコンパイル確認（CMake の arch_arm64_check）で担保する。
#include "core/Arch.h"

#include <cstdio>
#include <cstring>
#include <capstone/capstone.h>

using namespace ta2;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? "OK" : "NG", what);
  if (!ok) failures++;
}

int main() {
  auto arch = Arch::createForHost();
  printf("host arch: %s (cpuType=%d)\n", arch->name(), (int)arch->cpuType());
  printf("thread flavor=%d count=%u / debug flavor=%d count=%u\n",
         (int)arch->threadFlavor(), arch->threadStateCount(),
         (int)arch->debugFlavor(), arch->debugStateCount());

  // スレッド状態バッファ（flavor の大きさぶん）を用意
  std::vector<uint8_t> ts(arch->threadStateCount() * sizeof(natural_t), 0);
  std::vector<uint8_t> ds(arch->debugStateCount() * sizeof(natural_t), 0);

  // PC / SP の set/get
  arch->setPc(ts.data(), 0x1234);
  check(arch->pc(ts.data()) == 0x1234, "setPc/pc");

  // レジスタ一覧が取れる
  auto regs = arch->registers(ts.data());
  printf("registers: %zu 本 (先頭=%s, 末尾=%s)\n", regs.size(),
         regs.empty() ? "?" : regs.front().name.c_str(),
         regs.empty() ? "?" : regs.back().name.c_str());
  check(regs.size() >= 16, "register count");

  // 名前で書いて名前で読む（式 / setreg の経路）
  const char* reg = (arch->cpuType() == CPU_TYPE_X86_64) ? "rax" : "x0";
  bool w = arch->writeRegisterByName(ts.data(), reg, 0xDEADBEEF);
  uint64_t v = 0; bool r = arch->readRegisterByName(ts.data(), reg, v);
  check(w && r && v == 0xDEADBEEF, "write/readRegisterByName");

  // PC を名前からも読めるか
  uint64_t pcv = 0;
  const char* pcname = (arch->cpuType() == CPU_TYPE_X86_64) ? "rip" : "pc";
  check(arch->readRegisterByName(ts.data(), pcname, pcv) && pcv == 0x1234, "read PC by name");

  // ソフトブレイクのバイト列（x86:1byte CC / arm64:4byte BRK）
  auto bp = arch->softBreakBytes();
  printf("soft break bytes:");
  for (auto b : bp) printf(" %02X", b);
  printf(" (%zu byte)\n", bp.size());
  if (arch->cpuType() == CPU_TYPE_X86_64)
    check(bp.size() == 1 && bp[0] == 0xCC, "x86 INT3");
  else
    check(bp.size() == 4, "arm64 BRK is 4 byte");

  // 単一ステップの ON/OFF が状態を変える
  std::vector<uint8_t>& stepBuf = (arch->cpuType() == CPU_TYPE_X86_64) ? ts : ds;
  std::vector<uint8_t> before = stepBuf;
  arch->setSingleStep(stepBuf.data(), true);
  bool changed = (stepBuf != before);
  arch->setSingleStep(stepBuf.data(), false);
  check(changed && stepBuf == before, "setSingleStep toggles state");

  // HW ブレイクの set/clear
  check(arch->hwBreakCount() >= 1, "hwBreakCount");
  std::vector<uint8_t> dsBefore = ds;
  arch->setHwExecBreak(ds.data(), 0, 0x4000);
  bool hwSet = (ds != dsBefore);
  arch->clearHwExecBreak(ds.data(), 0);
  check(hwSet, "setHwExecBreak changes debug state");

  // HW ウォッチの set/clear（8 バイト書込監視）
  check(arch->hwWatchCount() >= 1, "hwWatchCount");
  std::vector<uint8_t> dsW = ds;
  arch->setHwWatch(ds.data(), 0, 0x8000, 8, /*onRead=*/false, /*onWrite=*/true);
  bool wSet = (ds != dsW);
  arch->clearHwWatch(ds.data(), 0);
  check(wSet, "setHwWatch changes debug state");

  // capstone が Arch の指定モードで動く（x86 を 1 命令デコード）
  csh h;
  if (cs_open((cs_arch)arch->csArch(), (cs_mode)arch->csMode(), &h) == CS_ERR_OK) {
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    const uint8_t x86ret[] = {0xc3};
    const uint8_t a64ret[] = {0xc0, 0x03, 0x5f, 0xd6};
    const uint8_t* code = (arch->cpuType() == CPU_TYPE_X86_64) ? x86ret : a64ret;
    size_t len = (arch->cpuType() == CPU_TYPE_X86_64) ? 1 : 4;
    cs_insn* insn = nullptr;
    size_t n = cs_disasm(h, code, len, 0x1000, 0, &insn);
    if (n > 0) {
      printf("disasm sanity: %s %s\n", insn[0].mnemonic, insn[0].op_str);
      check(strcmp(insn[0].mnemonic, "ret") == 0, "capstone decodes 'ret' in Arch mode");
      cs_free(insn, n);
    } else {
      check(false, "capstone disasm");
    }
    cs_close(&h);
  } else {
    check(false, "cs_open with Arch mode");
  }

  printf(failures == 0 ? "\nALL OK\n" : "\n%d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
