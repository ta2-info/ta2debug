#include "ui/TextReport.h"

#include <cstdio>
#include <cstring>

#include "core/SymbolRef.h" // formatDescribe / formatSymbolName（L1 の整形補助。v1 と同じ文字列）

namespace ta2 {
namespace ui {

using namespace engine;

namespace {
std::string hx12(uint64_t v) { char b[24]; snprintf(b, sizeof(b), "0x%012llx", (unsigned long long)v); return b; }
std::string hx(uint64_t v) { char b[24]; snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); return b; }
} // namespace

std::vector<std::string> TextReport::commandResult(const CommandResult& r) const {
  std::vector<std::string> out;
  std::string s = tr_.tr(r.message);
  // v1 が文末に "  " + describe(addr) を添えていたもの（ブレイク設定など）。
  if (r.where && r.where->hasImage) {
    std::string w = formatDescribe(*r.where);
    if (!w.empty()) s += (s.empty() ? "" : "  ") + w;
  }
  if (!s.empty()) out.push_back(s);
  return out;
}

std::string TextReport::stopReason(const StopInfo& si) const {
  // 例外は強調＋通常終了ヒント、syscall キャッチ・auto はそれぞれの Msg。notes は「  ／ 」連結。
  std::string s;
  if (!si.reason.empty()) {
    if (si.kind == StopKind::Exception)
      s = tr_.tr(Msg::of(MsgId::UiCrashHint).sub("reason", si.reason));
    else
      s = tr_.tr(si.reason);
  } else if (si.autoStop == AutoStop::Limit) {
    s = tr_.tr(Msg::of(MsgId::UiAutoLimit).num("n", si.autoSteps));
  } else if (si.autoStop == AutoStop::Interrupted) {
    s = tr_.tr(Msg::of(MsgId::UiAutoInterrupted).num("n", si.autoSteps));
  } else if (si.autoStop != AutoStop::None) {
    const char* k = si.autoStop == AutoStop::Call ? "call" : si.autoStop == AutoStop::Syscall ? "syscall" : "ret";
    s = tr_.tr(Msg::of(MsgId::UiAutoKind).str("kind", k).num("n", si.autoSteps));
  }
  std::string sep = tr_.tr(Msg::of(MsgId::UiNoteSep));
  for (const auto& n : si.notes) {
    std::string t = tr_.tr(n);
    if (!t.empty()) s += (s.empty() ? "" : sep) + t;
  }
  return s;
}

std::string TextReport::frame(const FrameView& f) const {
  std::string line = hx12(f.pc) + " ";
  std::string w = formatDescribe(f.sym);
  if (!w.empty()) line += " " + w;
  if (!f.note.empty()) line += "  " + tr_.tr(f.note);
  return line;
}

std::vector<std::string> TextReport::backtrace(const std::vector<FrameView>& frames) const {
  std::vector<std::string> out;
  out.push_back("backtrace:");
  for (const auto& f : frames) out.push_back("  " + frame(f));
  return out;
}

std::vector<std::string> TextReport::stopReport(const StopInfo& si,
                                                const std::vector<FrameView>& frames) const {
  std::vector<std::string> out;
  std::string w = formatDescribe(si.where);
  std::string head = "stopped @ " + (w.empty() ? hx(si.pc) : w);
  if (si.bpId >= 0) { char b[48]; snprintf(b, sizeof(b), "  [bp #%d, %lluhit]", si.bpId, (unsigned long long)si.bpHits); head += b; }
  if (si.watchId >= 0) { char b[96]; snprintf(b, sizeof(b), "  [watch #%d, %lluhit @%s]", si.watchId, (unsigned long long)si.watchHits, hx(si.watchAddr).c_str()); head += b; }
  out.push_back(head);
  if (si.source) {
    std::string t = si.source->text;
    size_t a0 = t.find_first_not_of(" \t"); t = (a0 == std::string::npos) ? std::string() : t.substr(a0);
    out.push_back("src: " + si.source->file + ":" + std::to_string(si.source->line) + (t.empty() ? "" : ("  " + t)));
  }
  std::string reason = stopReason(si);
  if (!reason.empty()) out.push_back(reason);
  for (const auto& l : backtrace(frames)) out.push_back(l);
  return out;
}

std::vector<std::string> TextReport::registers(const RegistersSnapshot& rs) const {
  std::vector<std::string> out;
  if (!rs.valid) { out.push_back(tr_.tr(Msg::of(MsgId::UiNotStopped))); return out; }
  out.push_back(tr_.tr(Msg::of(MsgId::UiRegsTitle)));
  for (const auto& r : rs.regs) {
    char b[80]; snprintf(b, sizeof(b), "%-7s = 0x%016llx", r.name.c_str(), (unsigned long long)r.value);
    out.push_back(b);
  }
  return out;
}

std::vector<std::string> TextReport::breakpoints(const std::vector<BreakpointInfo>& bps) const {
  std::vector<std::string> out;
  out.push_back(tr_.tr(Msg::of(MsgId::UiBpCount).num("n", (int64_t)bps.size())));
  std::string en = tr_.tr(Msg::of(MsgId::UiEnabled)), dis = tr_.tr(Msg::of(MsgId::UiDisabled));
  for (const auto& b : bps) {
    const char* k = b.kind == BpKind::Soft ? "SW" : b.kind == BpKind::HwExec ? "HW" : "WATCH";
    const char* on = b.enabled ? en.c_str() : dis.c_str();
    char line[256];
    if (b.kind == BpKind::Watch)
      snprintf(line, sizeof(line), "#%d  %-5s %s  %s  size=%d %s%s  hits=%llu  %s", b.id, k, hx12(b.addr).c_str(),
               on, b.watchSize, b.onRead ? "r" : "", b.onWrite ? "w" : "",
               (unsigned long long)b.hits, b.symbol.c_str());
    else
      snprintf(line, sizeof(line), "#%d  %-5s %s  %s  hits=%llu  %s", b.id, k, hx12(b.addr).c_str(),
               on, (unsigned long long)b.hits, b.symbol.c_str());
    out.push_back(line);
  }
  if (bps.empty()) out.push_back(tr_.tr(Msg::of(MsgId::UiBpNone)));
  return out;
}

std::vector<std::string> TextReport::images(const std::vector<ImageInfo>& imgs) const {
  std::vector<std::string> out;
  out.push_back(tr_.tr(Msg::of(MsgId::UiImgCount).num("n", (int64_t)imgs.size())));
  for (const auto& im : imgs) {
    char b[400]; snprintf(b, sizeof(b), "0x%012llx  +0x%08llx  %s%s", (unsigned long long)im.loadAddr,
                          (unsigned long long)im.textSize, im.name.c_str(), im.isMain ? "  [main]" : "");
    out.push_back(b);
  }
  return out;
}

std::vector<std::string> TextReport::segments(const SegmentList& sl) const {
  std::vector<std::string> out;
  if (!sl.found) { out.push_back(tr_.tr(Msg::of(MsgId::UiImageNotFound))); return out; }
  out.push_back(tr_.tr(Msg::of(MsgId::UiSegTitle).str("image", sl.image).str("slide", hx(sl.slide))));
  for (const auto& s : sl.segments) {
    char b[256]; snprintf(b, sizeof(b), "%-16s 0x%012llx  size 0x%llx", s.name.c_str(),
                          (unsigned long long)(s.vmaddr + sl.slide), (unsigned long long)s.vmsize);
    out.push_back(b);
  }
  if (sl.segments.empty()) out.push_back(tr_.tr(Msg::of(MsgId::UiSegNone)));
  return out;
}

std::vector<std::string> TextReport::symbols(const SymbolPage& sp) const {
  std::vector<std::string> out;
  if (!sp.found) { out.push_back(tr_.tr(Msg::of(MsgId::UiImageUnknown))); return out; }
  std::string head = tr_.tr(Msg::of(MsgId::UiSymCount).str("image", sp.image).num("n", (int64_t)sp.total));
  if (sp.truncated) head += tr_.tr(Msg::of(MsgId::UiSymTruncated));
  out.push_back(head);
  for (const auto& e : sp.entries) out.push_back(hx12(e.addr) + "  " + e.name);
  if (sp.entries.empty()) out.push_back(tr_.tr(Msg::of(MsgId::UiSymNone)));
  return out;
}

std::vector<std::string> TextReport::annotations(const AnnotationList& al) const {
  std::vector<std::string> out;
  if (!al.found) { out.push_back(tr_.tr(Msg::of(MsgId::UiImageNotFound))); return out; }
  std::string head = tr_.tr(Msg::of(MsgId::UiAnnCount).str("image", al.image).num("n", (int64_t)al.annots.size()));
  head += al.path.empty() ? tr_.tr(Msg::of(MsgId::UiAnnNoUuidSuffix)) : ("  [" + al.path + "]");
  out.push_back(head);
  std::string noname = tr_.tr(Msg::of(MsgId::UiAnnNoName));
  for (const auto& a : al.annots) {
    char b[512]; snprintf(b, sizeof(b), "0x%012llx  (unslid 0x%llx)  %s%s%s", (unsigned long long)(a.unslid + al.slide),
                          (unsigned long long)a.unslid, a.name.empty() ? noname.c_str() : a.name.c_str(),
                          a.memo.empty() ? "" : "   ; ", a.memo.c_str());
    out.push_back(b);
  }
  if (al.annots.empty()) out.push_back(tr_.tr(Msg::of(MsgId::UiAnnNone)));
  return out;
}

std::vector<std::string> TextReport::examine(const ExamineResult& ex) const {
  std::vector<std::string> out;
  switch (ex.kind) {
    case ExamineKind::Classic:
      for (const auto& l : ex.lines) {
        std::string s = hx12(l.addr) + " ";
        std::string asc;
        for (size_t i = 0; i < l.bytes.size(); i++) {
          bool ok = i < l.byteOk.size() && l.byteOk[i];
          char hb[8]; if (ok) snprintf(hb, sizeof(hb), " %02x", l.bytes[i]); else snprintf(hb, sizeof(hb), " **");
          s += hb;
          uint8_t c = l.bytes[i];
          asc += (ok && c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        out.push_back(s + "  |" + asc + "|");
      }
      break;
    case ExamineKind::Values:
      for (const auto& l : ex.lines) {
        std::string s = hx12(l.addr) + " ";
        for (const auto& v : l.values) s += "  " + (v.ok ? v.text : std::string("**"));
        out.push_back(s);
      }
      break;
    case ExamineKind::Strings:
      for (const auto& l : ex.lines) out.push_back(hx12(l.addr) + "  \"" + l.str + "\"");
      break;
    case ExamineKind::Insns:
      for (const auto& l : ex.lines) {
        std::string s = hx12(l.insn.address) + "  " + l.insn.mnemonic;
        if (!l.insn.opStr.empty()) s += " " + l.insn.opStr;
        out.push_back(s);
      }
      break;
  }
  if (out.empty()) out.push_back(tr_.tr(Msg::of(MsgId::UiExamineNone)));
  return out;
}

std::vector<std::string> TextReport::help() const {
  std::vector<std::string> out;
  out.push_back(tr_.tr(Msg::of(MsgId::UiHelpTitle)));
  for (const auto& sp : CommandInterpreter::table()) {
    std::string line = std::string("  ") + sp.name;
    if (sp.args && sp.args[0]) line += std::string(" ") + sp.args;
    if (sp.help != MsgId::None) {
      std::string h = tr_.tr(Msg::of(sp.help));
      if (!h.empty()) line += "   — " + h;
    }
    out.push_back(line);
  }
  return out;
}

} // namespace ui
} // namespace ta2
