#include "core/SyscallDecoder.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include <mach/message.h>

#include "core/Disassembler.h"
#include "core/SyscallTables.h"

namespace ta2 {

using namespace syscall_tables;

std::string SyscallCall::text() const {
  std::string s;
  for (size_t i = 0; i < args.size(); i++) { if (i) s += ", "; s += args[i].text; }
  return name + "(" + s + ")";
}

bool decodeSyscallAt(Target& target, const Breakpoints& bps, uint64_t pc,
                     const void* threadState, SyscallCall& out) {
  Arch& a = target.arch();

  // 停止位置の命令が syscall/svc か（ブレイク命令バイトは元に戻して判定）
  uint8_t code[16] = {}; size_t got = 0;
  if (!target.readMemory(pc, code, sizeof(code), &got) || got == 0) return false;
  bps.maskInto(pc, code, got);
  Disassembler dis(a);
  auto insns = dis.disasm(code, got, pc, 1);
  if (insns.empty() || insns[0].kind != InsnKind::Syscall) return false;

  // クラス判定（x86: rax 上位バイト Mach=1 / Unix=2。arm64: x16 が負なら Mach トラップ）は Arch 側。
  SyscallNo no = a.decodeSyscallNumber(threadState);
  bool mach = no.mach;
  uint64_t num = no.number;

  // 参照先ヘルパ
  auto cstr = [&](uint64_t p) -> std::string {
    if (!p) return "NULL";
    char buf[128] = {}; size_t n = 0;
    if (!target.readMemory(p, buf, sizeof(buf) - 1, &n) && n == 0) return "";
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
    target.readMemory(p, b.data(), want, &n);
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
    if (!target.readMemory(p, h, sizeof(h))) return hx;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s {id=%d size=%u %s rport=%s lport=%s}", hx, (int)h[5], h[1],
             decMsghBits(h[0]).c_str(), decPortName(h[2]).c_str(), decPortName(h[3]).c_str());
    std::string out = buf;

    // COMPLEX: ヘッダ(24B)直後の body(descriptor_count) とディスクリプタ列を解析（64bit ユーザ ABI）。
    if (h[0] & MACH_MSGH_BITS_COMPLEX) {
      uint32_t dcount = 0;
      if (target.readMemory(p + 24, &dcount, 4) && dcount && dcount < 256) {
        out += "  descriptors=" + std::to_string(dcount) + " [";
        uint64_t d = p + 28; // body の直後
        for (uint32_t i = 0; i < dcount && i < 8; i++) {
          uint8_t raw[16] = {};
          if (!target.readMemory(d, raw, sizeof(raw))) break;
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
  auto fmtArg = [&](const char* sig, int i) -> SyscallArg {
    SyscallArg arg;
    uint64_t v = a.syscallArgReg(threadState, i);
    arg.kind = sig[i];
    arg.value = v;
    char hex[24];
    switch (sig[i]) {
      case 'd': arg.text = std::to_string((long long)(int64_t)v); break;
      case 's': snprintf(hex, sizeof(hex), "0x%llx ", (unsigned long long)v); arg.text = std::string(hex) + cstr(v); break;
      case 'B': { uint64_t len = a.syscallArgReg(threadState, i + 1);
                  snprintf(hex, sizeof(hex), "0x%llx ", (unsigned long long)v); arg.text = std::string(hex) + buffer(v, len); break; }
      case 'O': arg.text = decOpenFlags(v); break;
      case 'P': arg.text = decProt(v); break;
      case 'M': arg.text = decMapFlags(v); break;
      case 'A': arg.text = decAccess(v); break;
      case 'W': arg.text = decWhence(v); break;
      case 'S': arg.text = decSignal(v); break;
      case 'G': arg.text = decMachOption(v); break;         // mach_msg オプション
      case 'H': arg.text = machHeader(v); break;            // mach_msg_header_t*
      case 'n': arg.text = decPortName(v); break;           // Mach ポート名
      case 'r': arg.text = decPortRight(v); break;          // Mach ポート権利
      case 'm': arg.text = decMsgType(v); break;            // メッセージ型 disposition
      case 'v': arg.text = decVmFlags(v); break;            // VM フラグ
      default:  snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)v); arg.text = hex; break; // x / p
    }
    return arg;
  };
  auto formatArgs = [&](const char* sig) {
    int n = (int)strlen(sig);
    for (int i = 0; i < n && i < 6; i++) out.args.push_back(fmtArg(sig, i));
  };

  out = SyscallCall();
  out.mach = mach;
  out.number = num;

  if (mach) { // Mach トラップ
    const Entry* mt = findMachTrap(num);
    if (mt) {
      out.known = true; out.hasSignature = true;
      out.name = mt->name;
      formatArgs(mt->sig);
    } else {
      out.name = "mach_trap#" + std::to_string((long long)num);
      formatArgs("xxx");
    }
    return true;
  }

  const Entry* info = findBsd(num);
  if (info) {
    out.known = true; out.hasSignature = true;
    out.name = info->name;
  } else {
    // 引数デコード表(kScalls)に無い syscall は、網羅表から名前だけ引く（例 sysctl）。
    if (const char* n = bsdName(num)) { out.known = true; out.name = n; }
    else out.name = std::string("syscall#") + std::to_string((long long)num);
  }
  formatArgs(info ? info->sig : "xxx");
  return true;
}

} // namespace ta2
