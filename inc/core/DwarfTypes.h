#pragma once

// DWARF の型情報（.debug_info / .debug_abbrev）を解釈し、構造体・共用体・列挙体・基本型などの
// レイアウト（メンバ名・オフセット・型・サイズ）を取り出す。用途は「あるアドレスのメモリを
// 指定した型として展開して見る」こと（gdb の p (struct Foo*)addr 相当）。行番号の DwarfLines と
// 同じく外部ライブラリ不使用の自前パース。Mach-O / DWARF はアーキ非依存。
//
// DwarfLines が読み込んだ __DWARF セクション群（dSYM なら 1 つ、dSYM 無しの -g 本体なら各 .o）を
// addSections で順に渡す。各セクション内の DIE を走査して型を集め、型参照（DW_AT_type）は
// そのセクション内の絶対オフセットで解決する（セクションをまたぐ参照は無い）。型名→型の索引を
// 作り、名前で引けるようにする。
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ta2 {

// __debug セクション群（必要なものだけ）。line_str は DWARF5 の行テーブル用。
// dSYM（__DWARF セグメント）と .o（無名セグメント内の __DWARF セクション）の両方から読む。
struct DwarfSections {
  std::vector<uint8_t> line, info, abbrev, str, lineStr;
};

// 型参照（DW_AT_type の指す先）。section=どのセクションの .debug_info 内か、off=その絶対オフセット。
struct DwTypeRef {
  int section = -1;
  uint64_t off = 0;
  bool valid = false;
};

struct DwMember {
  std::string name;      // メンバ名（無名の共用体/構造体メンバは空）
  uint64_t offset = 0;   // 構造体先頭からのバイトオフセット（DW_AT_data_member_location）
  DwTypeRef type;        // メンバの型
  bool isBit = false;    // ビットフィールドか
  int bitSize = 0;       // ビット幅（isBit のとき）
  int bitOffset = 0;     // データ先頭からのビット位置（DW_AT_data_bit_offset。DWARF4+）
};

struct DwEnumerator {
  std::string name;
  int64_t value = 0;
};

// 基底クラス（DW_TAG_inheritance）。多重継承では派生型が複数持つ。非仮想なら offset は
// 派生先頭からの定数バイトオフセット（単一継承の主基底は 0）。仮想基底は offset が実行時の
// vtable 依存で DWARF では定数にならないため isVirtual を立て、offset は未確定として扱う。
struct DwBase {
  DwTypeRef type;          // 基底型（それ自身も bases を持ちうる＝親の親へ再帰できる）
  uint64_t offset = 0;     // 非仮想のときの基底サブオブジェクトのオフセット
  bool isVirtual = false;  // 仮想継承（offset は未確定）
};

// 1 つの型。kind で種別、ref で参照先（ポインタ/typedef/const/volatile/配列要素）を表す。
struct DwType {
  enum Kind { Unknown, Base, Pointer, Struct, Union, Class, Typedef, Const, Volatile, Array, Enum };
  Kind kind = Unknown;
  std::string name;                    // 型名（基本型・集約型・enum・typedef）。無名は空
  uint64_t byteSize = 0;               // バイトサイズ（分かる範囲で）
  int encoding = 0;                    // Base: DW_ATE_*（signed/unsigned/float/boolean/char…）
  DwTypeRef ref;                       // Pointer/Typedef/Const/Volatile/Array要素/Enum下地 の参照
  std::vector<DwMember> members;       // Struct/Union/Class のメンバ
  std::vector<DwBase> bases;           // Struct/Class の基底クラス（継承。先頭＝宣言順）
  std::vector<DwEnumerator> enums;     // Enum の列挙子
  uint64_t arrayCount = 0;             // Array の要素数（0=不明/可変長）
  int section = -1;                    // この型が属するセクション索引
};

class DwarfTypes {
public:
  // セクション群（1 CU 以上）を解析して型を取り込む。dSYM は 1 回、debug map は .o ごとに呼ぶ。
  void addSections(const DwarfSections& s);
  bool empty() const { return byName_.empty(); }

  // 型名で集約型/基本型/enum/typedef を引く（"struct "/"union "/"enum " 接頭は呼び出し側で除く）。
  // 無ければ nullptr。
  const DwType* byName(const std::string& name) const;
  // 型参照を解決して DwType を返す（ポインタやメンバの型を辿る用）。無ければ nullptr。
  const DwType* at(const DwTypeRef& ref) const;
  // 既知の集約型/enum/typedef の名前を集める（型一覧表示用）。
  void names(std::vector<std::string>& out) const;

private:
  // セクションごとの「DIE 絶対オフセット → 型」表。参照はこの中で解決する。
  std::vector<std::unordered_map<uint64_t, DwType>> sections_;
  std::unordered_map<std::string, DwTypeRef> byName_; // 名前 → 型（先勝ち）
};

} // namespace ta2
