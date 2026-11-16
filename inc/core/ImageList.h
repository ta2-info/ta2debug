#pragma once

// 対象プロセスに読み込まれた Mach-O イメージ（実行ファイル・dylib）の一覧。
// dyld の all_image_infos から列挙し、各イメージの Mach-O ロードコマンドを解析する。
// dlopen 検知は infoArrayChangeTimestamp を見て再読込。Mach-O はアーキ非依存なので Arch 依存なし。
#include <cstdint>
#include <string>
#include <vector>

#include "core/Target.h"

namespace ta2 {

struct Segment {
  std::string name;
  uint64_t vmaddr = 0;   // ファイル上の vmaddr（実アドレスは vmaddr + slide）
  uint64_t vmsize = 0;
  uint64_t fileoff = 0;
  uint64_t filesize = 0;
  uint32_t initprot = 0; // 初期保護（VM_PROT_*）。__TEXT=r-x, __DATA=rw- など
};

struct Section {
  std::string seg;      // 所属セグメント名（__TEXT / __DATA / __DATA_CONST …）
  std::string name;     // セクション名（__text / __const / __cstring / __got …）
  uint64_t vmaddr = 0;  // ファイル上 vmaddr（実アドレス = vmaddr + slide）
  uint64_t vmsize = 0;
  uint32_t flags = 0;   // section_64.flags（S_* 種別＋属性。S_ATTR_PURE_INSTRUCTIONS 等）
};

struct Image {
  std::string path;
  std::string name;       // パスのファイル名部分
  uint64_t loadAddr = 0;  // 実際に読み込まれた先頭（スライド適用後）
  uint64_t slide = 0;     // loadAddr - __TEXT vmaddr
  uint64_t textSize = 0;  // __TEXT の vmsize（範囲判定用）
  uint64_t unwindVmaddr = 0; // __TEXT,__unwind_info のファイル上 vmaddr（0=無し）。CFI 用。
  uint64_t unwindSize = 0;   // 同サイズ
  uint64_t ehframeVmaddr = 0; // __TEXT,__eh_frame のファイル上 vmaddr（0=無し）。DWARF CFI 用。
  uint64_t ehframeSize = 0;
  // __TEXT,__stubs（シンボルスタブ）。スタブアドレス→本来のシンボル名の解決に使う。
  uint64_t stubVmaddr = 0; uint64_t stubSize = 0;
  uint32_t stubEntrySize = 0;     // section_64.reserved2（1 スタブのバイト数）
  uint32_t stubIndirectIndex = 0; // section_64.reserved1（間接シンボルテーブルの開始添字）
  bool hasDysymtab = false;       // LC_DYSYMTAB（間接シンボルテーブル）
  uint32_t indirectsymoff = 0, nindirectsyms = 0;
  bool hasUuid = false;
  uint8_t uuid[16] = {};
  bool hasSymtab = false;
  uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
  std::vector<Segment> segments;
  std::vector<Section> sections;  // 全セクション（正体判定 :what 用）

  // 実行時のアドレス範囲（__TEXT）
  bool containsText(uint64_t addr) const {
    return textSize && addr >= loadAddr && addr < loadAddr + textSize;
  }
  // __unwind_info / __eh_frame の実行時アドレス（スライド適用後）。無ければ 0。
  uint64_t unwindInfoAddr() const { return unwindVmaddr ? unwindVmaddr + slide : 0; }
  uint64_t ehFrameAddr() const { return ehframeVmaddr ? ehframeVmaddr + slide : 0; }
  // addr が __stubs（シンボルスタブ）内か。
  bool containsStub(uint64_t addr) const {
    return stubSize && addr >= stubVmaddr + slide && addr < stubVmaddr + slide + stubSize;
  }
  // addr（実アドレス）を含むセグメント／セクション（slide 適用済みで判定）。無ければ nullptr。
  const Segment* segmentAt(uint64_t addr) const {
    for (const auto& s : segments) {
      uint64_t b = s.vmaddr + slide;
      if (s.vmsize && addr >= b && addr < b + s.vmsize) return &s;
    }
    return nullptr;
  }
  const Section* sectionAt(uint64_t addr) const {
    for (const auto& s : sections) {
      uint64_t b = s.vmaddr + slide;
      if (s.vmsize && addr >= b && addr < b + s.vmsize) return &s;
    }
    return nullptr;
  }
};

class ImageList {
public:
  explicit ImageList(Target& t) : target_(t) {}

  bool reload();             // すべて読み直す
  bool refreshIfChanged();   // 一覧が変わっていれば読み直す（true=読み直した）

  const std::vector<Image>& images() const { return images_; }
  const Image* byAddress(uint64_t addr) const;             // アドレスを含むイメージ
  const Image* byName(const std::string& name) const;      // 名前（大小・拡張子を緩く照合）
  const Image* mainExecutable() const;                     // MH_EXECUTE

private:
  bool readStamp(uint64_t& stamp);
  bool parseMachO(uint64_t loadAddr, Image& out);
  std::string readCString(uint64_t addr, size_t maxLen = 1024);

  Target& target_;
  std::vector<Image> images_;
  uint64_t stamp_ = 0;
  bool hasStamp_ = false;
};

} // namespace ta2
