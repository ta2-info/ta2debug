#pragma once

// L1 Core（ta2core）: 停止位置の syscall / Mach トラップを構造化して返す。
// 由来は v1 Session::syscallAtStop（"write(1, 0x.., 14) \"hi\"" の文字列を直接返していた）。
// v2 では SyscallCall（名前 + 引数ごとの種別・生値・デコード済みテキスト）を返す（設計書 §4.2）。
// 番号レジスタのクラス判定（BSD / Mach）は Arch::decodeSyscallNumber に委ねる（§9.2。v1 は
// ここに x86 / arm64 の判定が混在していた）。名前・フラグ名は翻訳対象外（§8.1-4）。
#include <cstdint>
#include <string>
#include <vector>

#include "core/Breakpoints.h"
#include "platform/Target.h"

namespace ta2 {

struct SyscallArg {
  char kind = 'x';        // シグネチャ 1 文字（syscall_tables::Entry::sig と同じ。未知の表は 'x'）
  uint64_t value = 0;     // 引数レジスタの生値
  std::string text;       // デコード済み（"O_RDONLY|O_CLOEXEC (0x1000000)" / "0x.. \"path\"" など）
};

struct SyscallCall {
  bool mach = false;      // Mach トラップか（false = BSD syscall）
  uint64_t number = 0;    // クラスビット・符号を除いた番号
  bool known = false;     // 表から名前が引けたか（false なら name は "syscall#N" / "mach_trap#N"）
  bool hasSignature = false; // 引数シグネチャがあるか（無ければ args は汎用 3 個の 16 進）
  std::string name;
  std::vector<SyscallArg> args;

  // v1 syscallAtStop と同じ 1 行: "name(arg, arg, ...)"。
  std::string text() const;
};

// pc の命令が syscall / svc なら、threadState（そのスレッドの状態バッファ）の番号・引数レジスタを
// 解析して out に入れ true。該当しない・読めないなら false。ブレイク命令バイトは bps で元命令に
// 戻して判定する。ポインタ引数は対象メモリの参照先（文字列・バッファ・mach_msg ヘッダ）も読む。
bool decodeSyscallAt(Target& target, const Breakpoints& bps, uint64_t pc,
                     const void* threadState, SyscallCall& out);

} // namespace ta2
