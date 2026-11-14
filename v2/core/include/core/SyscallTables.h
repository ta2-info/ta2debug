#pragma once

// L1 Core（ta2core）: syscall / Mach トラップの表とフラグ名デコード。
// 由来は v1 src/core/Session.cpp の無名名前空間（設計書 §4.2 SyscallTables）。
// 表は言語非依存データ（syscall 名・フラグ名・ポート権利名などは翻訳対象外。§8.1-4）なので、
// デコード結果は識別子の文字列のまま返す。Arch にも依存しない（番号のクラス判定は
// Arch::decodeSyscallNumber が済ませる）。
#include <cstdint>
#include <string>

namespace ta2 {
namespace syscall_tables {

// 引数シグネチャつきの 1 件。sig は引数ごとの 1 文字:
//   BSD : d=10進, x=16進, p=ポインタ, s=C文字列(参照先), B=バッファ(長さは次の引数),
//         O=open フラグ, P=PROT_*, M=MAP_*, A=access モード, W=whence, S=シグナル番号
//   Mach: n=ポート名, r=ポート権利, m=メッセージ型 disposition, v=VM フラグ, P=VM 保護,
//         d=10進, x=16進, p=ポインタ, H=mach_msg ヘッダ, G=mach_msg オプション
struct Entry { int num; const char* name; const char* sig; };

// 引数デコード表（kScalls）から BSD syscall を引く。無ければ nullptr。
const Entry* findBsd(uint64_t num);
// 番号→名前だけの網羅表（SDK の全 SYS_*。SyscallNames.inc）。無ければ nullptr。
const char* bsdName(uint64_t num);
// Mach トラップ表（kMachTraps）。無ければ nullptr。
const Entry* findMachTrap(uint64_t num);

// --- フラグ / 列挙値の名前化（"A|B (0x..)" 形式など。v1 の dec* と同じ出力） ---
std::string decOpenFlags(uint64_t v);
std::string decProt(uint64_t v);
std::string decMapFlags(uint64_t v);
std::string decAccess(uint64_t v);
std::string decWhence(uint64_t v);
std::string decSignal(uint64_t v);
std::string decMachOption(uint64_t v);
std::string decPortName(uint64_t v);
std::string decPortRight(uint64_t v);
std::string decMsgType(uint64_t v);   // disposition
std::string decVmFlags(uint64_t v);
std::string decMsghBits(uint32_t bits); // mach_msg の msgh_bits（remote/local の disposition と COMPLEX）
const char* machDescType(uint32_t t);   // mach_msg ディスクリプタ種別名

} // namespace syscall_tables
} // namespace ta2
