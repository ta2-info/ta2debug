// Debugger の同期クエリ。v1 main.cpp の描画関数（drawDisassembly / drawRegisters / drawThreads /
// computeBacktrace の呼び出し部）と参照コマンド（img / seg / syms / anns / scwatch / dr / reg）が
// 対象から直接読んでいた部分を、スナップショットを返す形にしたもの。整形（文字列・色・桁揃え）はしない。
#include "engine/Debugger.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "core/Backtracer.h"
#include "core/BranchResolver.h"
#include "core/Disassembler.h"
#include "core/MemoryExaminer.h"

namespace ta2 {
namespace engine {

namespace {

// クエリ用のロック。対象を手放し中（ack 待ち・join 中）なら待たずに「取れなかった」を返す。
class QueryLock {
public:
  QueryLock(std::mutex& m, const std::atomic<bool>& teardown) {
    if (teardown.load()) return;
    lk_ = std::unique_lock<std::mutex>(m);
    ok_ = !teardown.load();
  }
  explicit operator bool() const { return ok_; }

private:
  std::unique_lock<std::mutex> lk_;
  bool ok_ = false;
};

std::string lowered(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

} // namespace

const uint8_t* Debugger::viewState(ThreadId t, std::vector<uint8_t>& buf) {
  if (!stoppedLocked()) return nullptr;
  thread_act_t stopT = session_.stepper().stopThread();
  if (!t || (thread_act_t)t == stopT) return session_.stepper().stopState().data();
  buf.assign(session_.arch().threadStateCount() * sizeof(natural_t), 0);
  if (!session_.target().getThreadState((thread_act_t)t, buf.data())) return session_.stepper().stopState().data();
  return buf.data();
}

bool Debugger::canRestart() {
  QueryLock lk(stateMu_, teardown_);
  return lk && !launchArgs_.empty();
}

ArchInfo Debugger::arch() {
  ArchInfo info;
  QueryLock lk(stateMu_, teardown_);
  if (!lk) return info;
  std::unique_ptr<Arch> host;
  Arch* a = nullptr;
  if (session_.isAttached()) a = &session_.arch();
  else { host = Arch::createForHost(); a = host.get(); }
  if (!a) return info;
  info.valid = true;
  info.name = a->name();
  std::vector<uint8_t> zero(a->threadStateCount() * sizeof(natural_t), 0);
  for (const auto& r : a->registers(zero.data())) info.registerNames.push_back(r.name);
  info.hwBreakCount = a->hwBreakCount();
  info.hwWatchCount = a->hwWatchCount();
  info.supportsSyntaxSwitch = a->supportsSyntaxSwitch();
  info.vectorFamily = a->vectorRegisterFamily();
  info.vectorCount = a->vectorRegisterCount();
  return info;
}

StopInfo Debugger::stopInfo() {
  QueryLock lk(stateMu_, teardown_);
  if (!lk || state_.load() != RunState::Stopped) return StopInfo();
  return stopInfo_;
}

std::vector<ThreadInfo> Debugger::threads() {
  std::vector<ThreadInfo> out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return out;
  auto ths = session_.target().threads();
  std::sort(ths.begin(), ths.end()); // task_threads() の並びは呼び出しごとに変わりうるので tid で安定化
  thread_act_t stopT = stoppedLocked() ? session_.stepper().stopThread() : 0;
  for (thread_act_t t : ths) {
    ThreadInfo ti;
    ti.id = (ThreadId)t;
    ti.isStopThread = stopT != 0 && t == stopT;
    out.push_back(ti);
  }
  return out;
}

RegistersSnapshot Debugger::registers(ThreadId t) {
  RegistersSnapshot snap;
  QueryLock lk(stateMu_, teardown_);
  if (!lk) return snap;
  std::vector<uint8_t> buf;
  const uint8_t* st = viewState(t, buf);
  if (!st) return snap;
  snap.valid = true;
  snap.thread = t ? t : (ThreadId)session_.stepper().stopThread();
  snap.regs = session_.arch().registers(st);
  return snap;
}

bool Debugger::readRegister(const std::string& name, uint64_t& out, ThreadId t) {
  QueryLock lk(stateMu_, teardown_);
  if (!lk) return false;
  std::vector<uint8_t> buf;
  const uint8_t* st = viewState(t, buf);
  if (!st) return false;
  return session_.arch().readRegisterByName(st, name, out);
}

VectorRegisters Debugger::vectorRegisters(ThreadId t) {
  VectorRegisters out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !stoppedLocked()) return out;
  thread_act_t th = t ? (thread_act_t)t : session_.stepper().stopThread();
  std::vector<uint8_t> fs;
  if (!session_.target().getFloatState(th, fs)) return out;
  out.fetched = true;
  out.regs = session_.arch().vectorRegisters(fs.data());
  return out;
}

DebugRegisters Debugger::debugRegisters(ThreadId t) {
  DebugRegisters out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !stoppedLocked()) return out;
  thread_act_t th = t ? (thread_act_t)t : session_.stepper().stopThread();
  std::vector<uint8_t> ds(session_.arch().debugStateCount() * sizeof(natural_t), 0);
  if (!session_.target().getDebugState(th, ds.data())) return out;
  out.fetched = true;
  out.regs = session_.arch().debugRegisters(ds.data());
  return out;
}

// 逆アセンブル（v1 drawDisassembly の算出部）。関数見出し・ソース行・ブレイク印・飛び先・syscall 注記を
// 行ごとの構造で返す。maxRows > 0 のときは v1 と同じ行割り当て（見出し・ソース行も 1 行）で打ち切る。
DisasmResult Debugger::disassemble(uint64_t addr, int count, DisasmOptions o, ThreadId t) {
  DisasmResult res;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached() || count <= 0) return res;
  Arch& a = session_.arch();
  std::vector<uint8_t> vbuf;
  const uint8_t* st = viewState(t, vbuf);
  uint64_t pc = 0;
  if (st) { pc = a.pc(st); res.hasPc = true; res.pc = pc; }
  uint64_t start = addr ? addr : pc;
  if (!start) return res;
  bool viewingStop = st && (!t || (thread_act_t)t == session_.stepper().stopThread());

  size_t want = std::min<size_t>(std::max<size_t>(512, (size_t)count * 16), 1u << 16);
  std::vector<uint8_t> code(want, 0); size_t got = 0;
  session_.target().readMemory(start, code.data(), code.size(), &got);
  session_.maskBreakpoints(start, code.data(), got);
  Disassembler dis(a, intel_.load());
  const Image* img = session_.images().byAddress(start);
  if (img) { res.image = img->name; res.imageBase = img->loadAddr; }
  auto insns = dis.disasm(code.data(), got, start, (size_t)count);

  int y = 0;
  const int height = o.maxRows;
  uint64_t lastFunc = 0; bool haveLast = false;
  std::string lastSrcFile; int lastSrcLine = -1;
  res.nextAddr = start;
  for (size_t i = 0; i < insns.size() && (height <= 0 || y < height); i++) {
    const Insn& in = insns[i];
    DisasmLine line;
    const Symbol* fn = img ? session_.symbols().byAddress(*img, in.address) : nullptr;
    if (fn && (!haveLast || lastFunc != fn->addr)) {
      line.funcHeader = session_.symbols().displayName(*fn);
      y++;
      if (height > 0 && y >= height) break;
    }
    haveLast = fn != nullptr; lastFunc = fn ? fn->addr : 0;
    // ソース行（dSYM の DWARF 行情報）。行が変わった所に付ける。
    if (o.withSource) {
      SourceLoc loc;
      if (session_.sourceLineFor(in.address, loc) && (loc.line != lastSrcLine || loc.file != lastSrcFile)) {
        lastSrcFile = loc.file; lastSrcLine = loc.line;
        line.source = loc;
        y++;
        if (height > 0 && y >= height) break;
      }
    }
    line.addr = in.address;
    line.size = in.size;
    line.mnemonic = in.mnemonic;
    line.opStr = in.opStr;
    line.kind = in.kind;
    if (fn) { line.hasFunc = true; line.funcOffset = in.address - fn->addr; }
    if (in.hasCallTarget) {
      BranchTarget bt;
      bt.addr = in.callTarget;
      bt.symbol = session_.symbolName(in.callTarget);
      line.target = bt;
    }
    bool cur = st && in.address == pc;
    line.isPc = cur;
    if (cur && viewingStop && in.kind == InsnKind::Syscall) { // これから呼ぶシステムコール
      SyscallCall sc;
      if (session_.syscallAtStop(sc)) line.syscall = sc;
    }
    // PC が間接 call/jmp（例 callq *%rax）なら、実行前でもレジスタ状況から飛び先が判る。
    if (cur && in.isBranch && in.indirect) {
      BranchTarget bt;
      bt.indirect = true;
      uint64_t tgt = 0;
      if (resolveIndirectTarget(session_.target(), in, st, tgt)) {
        bt.addr = tgt;
        bt.symbol = session_.symbolName(tgt);
      } else {
        bt.resolved = false;
      }
      line.target = bt;
    }
    if (const Breakpoint* ub = session_.breaks().userAt(in.address)) {
      line.bp.kind = !ub->hw ? BpMark::Soft : ub->watch ? BpMark::Watch : BpMark::Hw;
      line.bp.enabled = ub->enabled;
      line.bp.id = ub->id;
    }
    if (img) if (const Annot* an = session_.annotations().atExact(*img, in.address)) line.memo = an->memo;
    y++;
    res.nextAddr = in.address + in.size;
    res.lines.push_back(std::move(line));
  }
  return res;
}

// top の n 命令前のアドレスを返す。x86 は可変長で後方デコードが曖昧なので、手前から前方デコードして
// top にちょうど揃う命令列を探し、その n 個前を返す。見つからなければ top（v1 disasmBack）。
uint64_t Debugger::disasmBack(uint64_t top, int n) {
  if (n <= 0) return top;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return top;
  Disassembler dis(session_.arch(), intel_.load());
  const size_t kMaxLen = 15;                 // x86 命令の最大長
  size_t window = (size_t)n * kMaxLen + 32;  // n 命令を確実に含む手前幅
  for (size_t slack = 0; slack <= 24; slack++) { // 開始位置を 1 byte ずつずらし top に同期する並びを探す
    uint64_t start = top - (uint64_t)(window + slack);
    size_t len = window + slack + kMaxLen; std::vector<uint8_t> buf(len, 0); size_t got = 0;
    if (!session_.target().readMemory(start, buf.data(), len, &got) || got < window) continue;
    session_.maskBreakpoints(start, buf.data(), got);
    auto insns = dis.disasm(buf.data(), got, start, 4096);
    int topIdx = -1;
    for (int i = 0; i < (int)insns.size(); i++) {
      if (insns[i].address == top) { topIdx = i; break; }  // top が境界として現れた＝同期した
      if (insns[i].address > top) break;                   // top を跨いだ＝ずれている。slack を変えて再試行
    }
    if (topIdx >= 0) return insns[std::max(0, topIdx - n)].address;
  }
  return top; // 同期できない異常時は動かさない
}

uint64_t Debugger::insnBefore(uint64_t renderTop, uint64_t base) {
  if (!(renderTop && renderTop < base && base - renderTop <= (1u << 20))) return 0;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return 0;
  size_t span = (size_t)(base - renderTop) + 16;
  std::vector<uint8_t> buf(span, 0); size_t got = 0;
  session_.target().readMemory(renderTop, buf.data(), span, &got);
  session_.maskBreakpoints(renderTop, buf.data(), got);
  Disassembler dis(session_.arch(), intel_.load());
  auto in = dis.disasm(buf.data(), got, renderTop, span); // 1 命令≥1 バイトなので span で十分
  uint64_t prev = 0;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i].address == base) return prev;   // base の直前命令
    if (in[i].address > base) break;          // 跨いだ（通常起きない）
    prev = in[i].address;
  }
  return 0;
}

uint64_t Debugger::insnAfter(uint64_t a) {
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return a;
  uint8_t code[32] = {}; size_t got = 0;
  session_.target().readMemory(a, code, sizeof(code), &got);
  session_.maskBreakpoints(a, code, got);
  Disassembler dis(session_.arch(), intel_.load());
  auto in = dis.disasm(code, got, a, 2);
  return (in.size() >= 2) ? in[1].address : a;
}

std::vector<FrameView> Debugger::backtrace(ThreadId t, bool fast) {
  std::vector<FrameView> out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk) return out;
  std::vector<uint8_t> buf;
  const uint8_t* st = viewState(t, buf);
  if (!st) return out;
  Backtracer bt(session_.target(), session_.images(), session_.symbols(), session_.breaks());
  auto frames = bt.compute(st, fast);
  int idx = 0;
  for (const Frame& f : frames) {
    FrameView v;
    v.index = idx++;
    v.pc = f.pc; v.sp = f.sp; v.fp = f.fp;
    v.origin = f.origin;
    v.note = frameNoteMsg(f.origin);
    v.sym = session_.describe(f.pc);
    out.push_back(std::move(v));
  }
  return out;
}

MemoryBlock Debugger::readMemory(uint64_t addr, size_t len) {
  MemoryBlock mb;
  mb.addr = addr;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached() || len == 0) return mb;
  mb.bytes.assign(len, 0);
  size_t got = 0;
  if (!session_.target().readMemory(addr, mb.bytes.data(), len, &got) || got == 0) { mb.bytes.clear(); return mb; }
  session_.maskBreakpoints(addr, mb.bytes.data(), got); // ブレイク命令は元バイトで見せる
  mb.bytes.resize(got);
  mb.ok = true;
  return mb;
}

bool Debugger::examine(const std::string& spec, uint64_t addr, ExamineResult& out, Msg& err) {
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) { err = Msg::of(MsgId::ErrNeedAttach); return false; }
  MemoryExaminer ex(session_.target(), session_.breaks(), intel_.load());
  return ex.examine(spec, addr, out, err);
}

bool Debugger::classicDump(uint64_t addr, int bytes, ExamineResult& out, Msg& err) {
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) { err = Msg::of(MsgId::ErrNeedAttach); return false; }
  MemoryExaminer ex(session_.target(), session_.breaks(), intel_.load());
  return ex.classicDump(addr, bytes, out, err);
}

SymbolRef Debugger::describe(uint64_t addr) {
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) { SymbolRef r; r.addr = addr; return r; }
  return session_.describe(addr);
}

std::string Debugger::symbolName(uint64_t addr) {
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return std::string();
  return session_.symbolName(addr);
}

bool Debugger::sourceLine(uint64_t addr, SourceLoc& out) {
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return false;
  return session_.sourceLineFor(addr, out);
}

std::vector<BreakpointInfo> Debugger::breakpoints() {
  std::vector<BreakpointInfo> out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk) return out;
  bool att = session_.isAttached();
  for (const auto& b : session_.breaks().list()) {
    BreakpointInfo bi;
    bi.id = b.id; bi.addr = b.addr;
    bi.kind = BreakpointService::kindOf(b);
    bi.enabled = b.enabled; bi.hits = b.hits; bi.slot = b.slot;
    bi.watchSize = b.watchSize; bi.onRead = b.onRead; bi.onWrite = b.onWrite;
    if (att) bi.symbol = session_.symbolName(b.addr);
    out.push_back(std::move(bi));
  }
  return out;
}

PendingInfo Debugger::pendingBreakpoints() {
  PendingInfo p;
  QueryLock lk(stateMu_, teardown_);
  if (lk) p.count = bps_.pendingCount();
  return p;
}

std::vector<ImageInfo> Debugger::images() {
  std::vector<ImageInfo> out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return out;
  const Image* mainImg = session_.images().mainExecutable();
  for (const auto& im : session_.images().images()) {
    ImageInfo ii;
    ii.name = im.name; ii.path = im.path;
    ii.loadAddr = im.loadAddr; ii.slide = im.slide; ii.textSize = im.textSize;
    ii.uuid = BreakpointService::uuidHexOf(im);
    ii.isMain = (&im == mainImg);
    out.push_back(std::move(ii));
  }
  return out;
}

SegmentList Debugger::segments(const std::string& image) {
  SegmentList out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return out;
  const Image* im = image.empty() ? session_.images().mainExecutable() : session_.images().byName(image);
  if (!im) return out;
  out.found = true;
  out.image = im->name; out.slide = im->slide;
  out.segments = im->segments;
  return out;
}

SymbolPage Debugger::symbols(const std::string& image, const std::string& filter, size_t limit) {
  SymbolPage out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return out;
  const Image* im = image.empty() ? session_.defaultLibImage() : session_.images().byName(image);
  if (!im) return out;
  out.found = true;
  out.image = im->name;
  std::string lf = lowered(filter);
  const auto& syms = session_.symbols().forImage(*im);
  for (const auto& s : syms) {
    const std::string& dn = session_.symbols().displayName(s); // 遅延デマングル（この一覧ぶんだけ）
    if (!lf.empty()) {
      std::string hay = lowered(dn + " " + s.mangled);
      if (hay.find(lf) == std::string::npos) continue;
    }
    out.total++;
    if (limit && out.entries.size() >= limit) { out.truncated = true; continue; }
    out.entries.push_back(SymbolEntry{s.addr, dn});
  }
  return out;
}

AnnotationList Debugger::annotations(const std::string& image) {
  AnnotationList out;
  QueryLock lk(stateMu_, teardown_);
  if (!lk || !session_.isAttached()) return out;
  const Image* im = image.empty() ? session_.images().mainExecutable() : session_.images().byName(image);
  if (!im) return out;
  if (!jobs_.running()) session_.annotations().refresh(*im); // 外部編集の取り込み（探索中は触らない）
  out.found = true;
  out.image = im->name; out.slide = im->slide;
  out.path = Annotations::pathFor(*im);
  out.annots = session_.annotations().list(*im);
  return out;
}

std::string Debugger::defaultLib() {
  QueryLock lk(stateMu_, teardown_);
  if (!lk) return std::string();
  return session_.defaultLib();
}

SyscallWatchStatus Debugger::syscallWatch() {
  SyscallWatchStatus st;
  QueryLock lk(stateMu_, teardown_);
  if (!lk) return st;
  SyscallMonitor& m = session_.syscalls();
  st.active = m.active();
  st.stopMode = st.active && m.stopMode();
  st.traceMode = m.traceMode();
  st.filter = m.filter();
  st.total = m.total();
  st.counts = m.counts();
  std::sort(st.counts.begin(), st.counts.end(),
            [](const std::pair<std::string, uint64_t>& x, const std::pair<std::string, uint64_t>& y) {
              return x.second > y.second; // 回数の多い順
            });
  st.trace = m.trace();
  std::reverse(st.trace.begin(), st.trace.end()); // 新しい順
  return st;
}

WarmStatus Debugger::symbolWarming() {
  WarmStatus w;
  // SymbolWarmer の進捗は atomic なのでロック不要（対象を手放し中でも読める）。
  w.active = session_.warmer().active();
  w.done = session_.warmer().done();
  w.total = session_.warmer().total();
  return w;
}

std::vector<ProcessInfo> Debugger::processes() { return listProcesses(); }

bool Debugger::syscallNames(const std::string& filter, SyscallNameList& out, Msg& err) {
  return SyscallMonitor::listNames(filter, out.bsd, out.mach, err);
}

} // namespace engine
} // namespace ta2
