#include "core/MemoryExaminer.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace ta2 {

// 1 個の値を書式 fmt・サイズ size で整形（x:16進 d:符号付 u:符号無 o:8進 t:2進 c:文字 a:アドレス f:浮動小数）
std::string formatExamineValue(uint64_t v, char fmt, int size) {
  if (size < 8) v &= (size == 1 ? 0xFFULL : size == 2 ? 0xFFFFULL : 0xFFFFFFFFULL);
  char b[96];
  switch (fmt) {
    case 'd': { // 符号付（size から符号拡張）
      uint64_t sv = v;
      if (size < 8) { uint64_t sign = 1ULL << (size*8 - 1); if (v & sign) sv = v | ~((1ULL<<(size*8))-1); }
      snprintf(b, sizeof(b), "%lld", (long long)sv); break;
    }
    case 'u': snprintf(b, sizeof(b), "%llu", (unsigned long long)v); break;
    case 'o': snprintf(b, sizeof(b), "0%llo", (unsigned long long)v); break;
    case 't': { std::string s; for (int i = size*8 - 1; i >= 0; i--) { s += ((v>>i)&1) ? '1' : '0'; if (i && i%8==0) s += ' '; } return s; }
    case 'c': { uint8_t c = (uint8_t)(v & 0xFF);
      if (c >= 0x20 && c < 0x7f) snprintf(b, sizeof(b), "0x%02x '%c'", c, (char)c);
      else snprintf(b, sizeof(b), "0x%02x", c); break; }
    case 'a': snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); break; // シンボル注記は表示側
    case 'f': { if (size == 4) { float f; uint32_t u=(uint32_t)v; memcpy(&f,&u,4); snprintf(b,sizeof(b),"%g",(double)f); } else { double d; memcpy(&d,&v,8); snprintf(b,sizeof(b),"%g",d); } break; }
    case 'x': default: snprintf(b, sizeof(b), "0x%0*llx", size*2, (unsigned long long)v); break;
  }
  return b;
}

// gdb 風の書式指定。spec = "[個数][書式][サイズ]"（例: 4xg / 8c / 10i / s）。
bool parseExamineSpec(const std::string& spec, ExamineSpec& out, Msg& err) {
  int count = 0; char fmt = 'x'; int size = 0; size_t i = 0;
  while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') { count = count*10 + (spec[i]-'0'); i++; }
  for (; i < spec.size(); i++) {
    char c = spec[i];
    if (c=='b') size=1; else if (c=='h') size=2; else if (c=='w') size=4; else if (c=='g') size=8;
    else if (c=='x'||c=='d'||c=='u'||c=='o'||c=='t'||c=='c'||c=='i'||c=='s'||c=='f'||c=='a') fmt=c;
    else { err = Msg::of(MsgId::ErrExamineBadFormat).str("ch", std::string(1, c)); return false; }
  }
  if (count <= 0) count = (fmt=='c') ? 32 : (fmt=='s') ? 4 : 8;
  if (size <= 0) size = (fmt=='c') ? 1 : 8;
  if (fmt=='c') size = 1;
  if (fmt=='f' && size < 4) size = 8;
  out.count = count; out.fmt = fmt; out.size = size;
  return true;
}

bool MemoryExaminer::examine(const std::string& spec, uint64_t addr, ExamineResult& out, Msg& err) {
  ExamineSpec s;
  if (!parseExamineSpec(spec, s, err)) return false;
  return examine(s, addr, out, err);
}

bool MemoryExaminer::examine(const ExamineSpec& spec, uint64_t addr, ExamineResult& out, Msg& err) {
  int count = spec.count; char fmt = spec.fmt; int size = spec.size;
  ExamineResult res;
  res.addr = addr; res.fmt = fmt; res.size = size; res.count = count;

  if (fmt == 'i') { // 逆アセンブル count 命令
    res.kind = ExamineKind::Insns;
    std::vector<uint8_t> code((size_t)count * 16 + 16, 0); size_t got = 0;
    target_.readMemory(addr, code.data(), code.size(), &got);
    bps_.maskInto(addr, code.data(), got);
    Disassembler dis(target_.arch(), intel_);
    auto insns = dis.disasm(code.data(), got, addr, count);
    for (auto& in : insns) {
      ExamineLine l;
      l.addr = in.address;
      l.insn = std::move(in);
      res.lines.push_back(std::move(l));
    }
    res.nextAddr = res.lines.empty() ? addr
                 : (res.lines.back().insn.address + res.lines.back().insn.size); // 継続先
    out = std::move(res);
    return true;
  }

  if (fmt == 's') { // null 終端文字列を count 本
    res.kind = ExamineKind::Strings;
    for (int k = 0; k < count; k++) {
      std::string str; uint8_t c = 0; uint64_t a = addr; int n = 0;
      for (; n < 4096; n++) { if (!target_.readMemory(a + n, &c, 1)) { c = 0; break; } if (c == 0) break;
        str += (c >= 0x20 && c < 0x7f) ? (char)c : '.'; }
      ExamineLine l;
      l.addr = a;
      l.str = std::move(str);
      res.lines.push_back(std::move(l));
      addr = a + n + 1;
    }
    res.nextAddr = addr; // 最後の文字列の次（継続先）
    out = std::move(res);
    return true;
  }

  // 数値 / 文字
  res.kind = ExamineKind::Values;
  size_t total = (size_t)count * size;
  std::vector<uint8_t> buf(total, 0); size_t got = 0;
  if (!target_.readMemory(addr, buf.data(), total, &got) || got == 0) {
    err = Msg::of(MsgId::ErrMemUnreadable).hex("addr", addr);
    return false;
  }
  int avail = (int)(got / (size_t)size); if (avail < count) count = avail;
  res.count = count;
  int perLine = (fmt=='a') ? 1 : size>=8 ? 2 : size>=4 ? 4 : size>=2 ? 8 : 8;
  for (int idx = 0; idx < count; ) {
    ExamineLine l;
    l.addr = addr + (size_t)idx*size;
    for (int j = 0; j < perLine && idx < count; j++, idx++) {
      uint64_t v = 0; for (int bb = 0; bb < size; bb++) v |= (uint64_t)buf[(size_t)idx*size + bb] << (8*bb);
      ExamineValue ev;
      ev.value = v;
      ev.text = formatExamineValue(v, fmt, size);
      l.values.push_back(std::move(ev));
    }
    res.lines.push_back(std::move(l));
  }
  res.nextAddr = addr + (size_t)count * size; // 継続先
  out = std::move(res);
  return true;
}

// 従来の 16 進＋ASCII ダンプ。bytes バイトを読み、16 バイトごとの行と継続先アドレスを返す。
bool MemoryExaminer::classicDump(uint64_t addr, int bytes, ExamineResult& out, Msg& err) {
  if (bytes < 1) bytes = 1;
  if (bytes > 4096) bytes = 4096;
  std::vector<uint8_t> buf(bytes, 0); size_t got = 0;
  if (!target_.readMemory(addr, buf.data(), bytes, &got) || got == 0) {
    err = Msg::of(MsgId::ErrMemUnreadable).hex("addr", addr);
    return false;
  }
  ExamineResult res;
  res.kind = ExamineKind::Classic;
  res.addr = addr; res.fmt = 'x'; res.size = 1; res.count = (int)got; res.bytesRead = got;
  for (size_t i = 0; i < got; i += 16) {
    ExamineLine l;
    l.addr = addr + i;
    for (size_t j = 0; j < 16 && i + j < got; j++) l.bytes.push_back(buf[i + j]);
    res.lines.push_back(std::move(l));
  }
  res.nextAddr = addr + got; // 継続先
  out = std::move(res);
  return true;
}

// HEX 文字列（空白無視）→ バイト列。空・奇数長・不正文字で false。
bool parseHexBytes(const std::string& in, std::vector<uint8_t>& out) {
  std::string h; for (char c : in) if (!std::isspace((unsigned char)c)) h += c;
  if (h.empty() || h.size() % 2) return false;
  auto nib = [](char c) -> int { if (c>='0'&&c<='9') return c-'0'; if (c>='a'&&c<='f') return c-'a'+10; if (c>='A'&&c<='F') return c-'A'+10; return -1; };
  for (size_t i = 0; i < h.size(); i += 2) { int hi = nib(h[i]), lo = nib(h[i+1]); if (hi<0||lo<0) return false; out.push_back((uint8_t)(hi*16+lo)); }
  return true;
}

} // namespace ta2
