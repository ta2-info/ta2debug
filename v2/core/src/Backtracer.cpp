#include "core/Backtracer.h"

#include <cstring>
#include <string>

#include "core/Unwinder.h"

namespace ta2 {

Msg frameNoteMsg(FrameOrigin origin) {
  switch (origin) {
    case FrameOrigin::EntryStack: return Msg::of(MsgId::BtNoteFromSp);
    case FrameOrigin::EntryLr:    return Msg::of(MsgId::BtNoteFromLr);
    case FrameOrigin::FpChain:    return Msg::of(MsgId::BtNoteFpChain);
    case FrameOrigin::StackScan:  return Msg::of(MsgId::BtNoteStackScan);
    case FrameOrigin::Current:
    case FrameOrigin::Cfi:        break;
  }
  return Msg();
}

// バックトレース領域（フレームポインタ鎖。[fp]=saved fp, [fp+8]=return。両アーキ共通）
// アドレスがいずれかのイメージの実行領域（__TEXT）内か。戻りアドレスの妥当性判定に使う。
bool Backtracer::addrIsCode(uint64_t x) const {
  if (!x) return false;
  for (const auto& im : images_.images()) if (im.containsText(x)) return true;
  return false;
}

// x が「call/bl の直後（＝正当な戻りアドレス）」らしいか。
// x の直前に、ちょうど x で終わる call 命令が置かれているかを逆アセンブルで確かめる。
// スタック走査での誤検出（たまたまコードを指すデータ）を弾くための強い判定。
bool Backtracer::addrIsCallReturn(Disassembler& dis, uint64_t x) {
  if (!addrIsCode(x)) return false;
  for (int len = 2; len <= 8; len++) {          // x86 call は 2〜7B / arm64 bl は 4B
    uint64_t s = x - (uint64_t)len;
    if (!addrIsCode(s)) continue;
    uint8_t buf[8] = {};
    if (!target_.readMemory(s, buf, (size_t)len)) continue;
    auto in = dis.disasm(buf, (size_t)len, s, 1);
    if (!in.empty() && in[0].size == len && in[0].kind == InsnKind::Call) return true;
  }
  return false;
}

// from 以上のスタックを上へ走査し、最初の「戻りアドレスらしい」語を返す（見つけた語のアドレスも）。
bool Backtracer::scanStackForReturn(Disassembler& dis, uint64_t from, uint64_t& outRet, uint64_t& outSlot) {
  uint64_t base = (from + 7) & ~(uint64_t)7; // 8 境界
  uint8_t buf[8192];
  size_t got = 0;
  for (size_t win = sizeof(buf); win >= 256; win /= 2) // 末尾ガードページ手前は小さく再試行
    if (target_.readMemory(base, buf, win, &got)) { got = win; break; }
  if (!got) return false;
  for (size_t off = 0; off + 8 <= got; off += 8) {
    uint64_t v = 0; memcpy(&v, buf + off, 8);
    if (addrIsCallReturn(dis, v)) { outRet = v; outSlot = base + off; return true; }
  }
  return false;
}

std::vector<Frame> Backtracer::compute(const void* st, bool fast) {
  std::vector<Frame> frames;
  Arch& a = target_.arch();
  Disassembler dis(a);
  MemReader reader = [this](uint64_t addr, void* buf, size_t len) {
    return target_.readMemory(addr, buf, len);
  };
  Unwinder unw(images_, a, reader);

  const Image* mainExe = images_.mainExecutable();
  UnwindRegs regs;
  regs.pc = a.pc(st); // 表示中スレッドの PC（停止スレッドなら stopPc と一致）
  regs.sp = a.sp(st);
  regs.fp = a.fp(st);
  regs.lr = a.linkRegister(st); // arm64 の leaf frameless 用（x86 は 0）
  uint64_t scanBase = regs.sp;  // スタック走査の開始点
  bool scanning = false;        // いちど連鎖が壊れたら以降は走査で辿る
  bool sawMain = false;
  FrameOrigin pending = FrameOrigin::Current; // 次に積むフレームの由来（v1 の pendingNote）

  for (int depth = 0; depth < 64; depth++) {
    Frame f;
    f.pc = regs.pc; f.sp = regs.sp; f.fp = regs.fp; f.origin = pending;
    frames.push_back(f);
    pending = FrameOrigin::Cfi; // 注記なしの既定（CFI で進んだ段）

    const Image* im = images_.byAddress(regs.pc);
    if (im && im == mainExe) sawMain = true;
    // メインを通過して dyld（start）まで達したら実質の最外フレーム。この 1 枚を出して打ち切る
    //（その上はローダのブート領域で、辿っても残骸＝偽フレームになるだけ）。
    if (sawMain && im && im != mainExe && im->name.find("dyld") != std::string::npos) break;

    // (1) まだ自分のフレームを作っていない位置（プロローグ未実行）で止まっている場合。
    //     この時点では rbp/x29 は呼び出し元のままなので、CFI/fp の frame ルールを当てると
    //     直近の呼び出し元を飛ばす。戻り先を [sp](x86)/LR(arm64) から 1 段復元する。該当するのは:
    //       (a) IP=0 等、どのイメージにも属さない異常ジャンプ
    //       (b) 関数の最初の命令（:b foo で foo+0 停止、call した直後）
    //       (c) unwind 情報の無いコード（dylib 遅延スタブ等の `jmp *ptr` サンク。フレームを作らない）
    if (depth == 0) {
      uint32_t enc = 0; uint64_t fstart = 0;
      bool hasEnc = addrIsCode(regs.pc) && unw.encodingFor(regs.pc, enc, fstart);
      // 関数入口の判定。compact unwind は同じエンコーディングの隣接関数を 1 エントリに
      // まとめる（例: -O2 で work と main が同一 FRAME エンコーディング）ため、fstart だけでは
      // 後ろ側の関数の入口を見逃す。シンボル先頭とも照合する。
      bool atEntry = hasEnc && regs.pc == fstart;
      if (hasEnc && !atEntry) {
        if (const Image* eim = images_.byAddress(regs.pc))
          if (const Symbol* s = symbols_.byAddress(*eim, regs.pc))
            atEntry = (s->addr == regs.pc);
      }
      // (b') arm64 のプロローグ途中（sub sp / stp x29,x30 は済んだが add x29,sp が未実行）。
      //      x29 はまだ呼び出し元のものなので FRAME ルールだと 1 段飛ぶ。戻り先は LR に残っている。
      //      ※ v1 から引き継いだ arm64 固有の判定（"x29, sp" の綴り比較）。戻り先が LR にある
      //        アーキ（!retAddrAtEntryOnStack）でだけ通る。Arch への引き上げは §9 の残課題。
      if (hasEnc && !atEntry && !a.retAddrAtEntryOnStack()) {
        uint64_t fn = fstart;
        if (const Image* eim = images_.byAddress(regs.pc))
          if (const Symbol* s = symbols_.byAddress(*eim, regs.pc)) fn = s->addr;
        if (fn < regs.pc && regs.pc - fn <= 64) {
          uint8_t buf[64] = {};
          size_t len = sizeof(buf); // プロローグは先頭 16 命令以内に収まる前提で探す
          if (target_.readMemory(fn, buf, len)) {
            bps_.maskInto(fn, buf, len); // bp の BRK を元命令に戻して判定
            for (const Insn& in : dis.disasm(buf, len, fn, 16)) {
              bool setsFp = (in.mnemonic == "add" || in.mnemonic == "mov") &&
                            in.opStr.rfind("x29, sp", 0) == 0;
              if (setsFp) { atEntry = (in.address >= regs.pc); break; } // pc 以降なら未確立
            }
          }
        }
      }
      bool certain = !addrIsCode(regs.pc) || atEntry; // (a)(b): [sp] は確実に戻り先
      bool stub = addrIsCode(regs.pc) && !hasEnc;                           // (c): スタブ/サンク疑い
      if (certain || stub) {
        uint64_t ret = 0;
        if (a.retAddrAtEntryOnStack()) {
          uint64_t top = 0;
          if (target_.readMemory(regs.sp, &top, sizeof(top))) ret = top;
        } else ret = regs.lr;
        // (c) は確証が無いので「call/bl 直後らしさ」で誤検出を弾く。(a)(b) は addrIsCode で足りる。
        bool ok = certain ? addrIsCode(ret) : addrIsCallReturn(dis, ret);
        if (ok) {
          if (a.retAddrAtEntryOnStack()) regs.sp += 8; // 戻り先をポップした後の sp
          regs.pc = ret;                               // fp は据え置き（＝呼び出し元フレーム）
          pending = a.retAddrAtEntryOnStack() ? FrameOrigin::EntryStack   // ← [sp] から復元
                                              : FrameOrigin::EntryLr;     // ← lr から復元
          scanBase = regs.sp;
          continue;
        }
      }
    }

    // (2) 本命: CFI（compact unwind）で正確に 1 段。RBP フレームも frameless も正しく辿れる。
    if (!scanning && addrIsCode(regs.pc)) {
      UnwindRegs next;
      // sp は通常単調増加。ただし arm64 の leaf frameless（stackSize=0）は最内フレームでのみ
      // sp 不変のまま LR へ戻るのが正しい（next.lr=0 なので 2 段目以降で再発せずループしない）。
      bool stepped = unw.step(regs, next);
      bool spOk = next.sp > regs.sp || (depth == 0 && next.sp == regs.sp && next.pc != regs.pc);
      if (stepped && next.pc && addrIsCode(next.pc) && spOk) {
        scanBase = regs.fp ? regs.fp + 16 : next.sp;
        regs = next;
        continue;
      }
    }

    // (3) フォールバック: __unwind_info が無い/DWARF 等。フレームポインタ(rbp/x29)連鎖で次段。
    if (!scanning && regs.fp) {
      uint64_t slot[2] = {};
      if (target_.readMemory(regs.fp, slot, sizeof(slot)) &&
          slot[0] > regs.fp && addrIsCode(slot[1])) {
        scanBase = regs.fp + 16;
        regs.sp = regs.fp + 16; regs.pc = slot[1]; regs.fp = slot[0];
        pending = FrameOrigin::FpChain; // ← fp 連鎖
        continue;
      }
    }

    // (4) 特別対応: 連鎖が切れた／呼び出し元が不正。スタック走査で戻りアドレスを復元（推定）。
    //     常に sp から上へ（fp は壊れている可能性があるので当てにしない）。
    if (fast) break; // 別スレッド表示時は高コストな走査を避け、ここで打ち切る
    uint64_t ret = 0, slotAt = 0;
    uint64_t from = scanning ? scanBase : regs.sp;
    if (scanStackForReturn(dis, from, ret, slotAt)) {
      regs.pc = ret; regs.fp = 0; regs.sp = slotAt + 8;
      scanning = true; scanBase = slotAt + 8;
      pending = FrameOrigin::StackScan; // ← スタック走査で復元(推定)
      continue;
    }
    break; // これ以上は辿れない
  }
  return frames;
}

} // namespace ta2
