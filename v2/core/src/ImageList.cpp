#include "core/ImageList.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <mach/task.h>
#include <mach/task_info.h>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>

namespace ta2 {

std::string ImageList::readCString(uint64_t addr, size_t maxLen) {
  std::string s;
  char buf[128];
  while (s.size() < maxLen) {
    size_t got = 0;
    if (!target_.readMemory(addr + s.size(), buf, sizeof(buf), &got) && got == 0) break;
    size_t n = got ? got : sizeof(buf);
    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '\0') return s;
      s.push_back(buf[i]);
    }
    if (got == 0) break;
  }
  return s;
}

bool ImageList::readStamp(uint64_t& stamp) {
  if (!target_.isAttached()) return false;
  task_dyld_info_data_t info = {};
  mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
  if (task_info(target_.task(), TASK_DYLD_INFO, (task_info_t)&info, &cnt) != KERN_SUCCESS) return false;

  struct dyld_all_image_infos all = {};
  size_t want = std::min(sizeof(all), (size_t)info.all_image_info_size);
  if (want < offsetof(struct dyld_all_image_infos, dyldImageLoadAddress)) return false;
  if (!target_.readMemory(info.all_image_info_addr, &all, want)) return false;
  if (all.infoArray == nullptr) return false; // 更新中

  if (all.version >= 15 && want >= offsetof(struct dyld_all_image_infos, infoArrayChangeTimestamp) + sizeof(uint64_t))
    stamp = all.infoArrayChangeTimestamp;
  else
    stamp = all.infoArrayCount;
  return true;
}

bool ImageList::refreshIfChanged() {
  uint64_t s = 0;
  if (!readStamp(s)) return false;
  if (hasStamp_ && s == stamp_) return false;
  return reload();
}

bool ImageList::reload() {
  if (!target_.isAttached()) return false;

  task_dyld_info_data_t info = {};
  mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
  if (task_info(target_.task(), TASK_DYLD_INFO, (task_info_t)&info, &cnt) != KERN_SUCCESS) return false;

  struct dyld_all_image_infos all = {};
  size_t want = std::min(sizeof(all), (size_t)info.all_image_info_size);
  if (!target_.readMemory(info.all_image_info_addr, &all, want)) return false;
  if (all.infoArray == nullptr || all.infoArrayCount == 0) return false;

  std::vector<struct dyld_image_info> arr(all.infoArrayCount);
  if (!target_.readMemory((uint64_t)(uintptr_t)all.infoArray, arr.data(),
                          arr.size() * sizeof(struct dyld_image_info))) {
    return false;
  }

  std::vector<Image> out;
  for (auto& di : arr) {
    Image img;
    img.loadAddr = (uint64_t)(uintptr_t)di.imageLoadAddress;
    img.path = readCString((uint64_t)(uintptr_t)di.imageFilePath);
    size_t slash = img.path.find_last_of('/');
    img.name = (slash == std::string::npos) ? img.path : img.path.substr(slash + 1);
    if (parseMachO(img.loadAddr, img)) out.push_back(std::move(img));
  }

  // dyld 自身は infoArray に含まれない場合があるので足す
  if (all.dyldImageLoadAddress) {
    uint64_t da = (uint64_t)(uintptr_t)all.dyldImageLoadAddress;
    bool found = false;
    for (auto& i : out) if (i.loadAddr == da) { found = true; break; }
    if (!found) {
      Image img; img.loadAddr = da; img.path = "/usr/lib/dyld"; img.name = "dyld";
      if (parseMachO(da, img)) out.push_back(std::move(img));
    }
  }

  std::sort(out.begin(), out.end(), [](const Image& a, const Image& b) { return a.loadAddr < b.loadAddr; });
  images_ = std::move(out);

  uint64_t s = 0;
  hasStamp_ = readStamp(s);
  stamp_ = s;
  return true;
}

bool ImageList::parseMachO(uint64_t loadAddr, Image& out) {
  struct mach_header_64 h = {};
  if (!target_.readMemory(loadAddr, &h, sizeof(h))) return false;
  if (h.magic != MH_MAGIC_64) return false;

  std::vector<uint8_t> cmds(h.sizeofcmds);
  if (!target_.readMemory(loadAddr + sizeof(h), cmds.data(), cmds.size())) return false;

  uint64_t textVmaddr = 0;
  bool haveText = false;
  const uint8_t* p = cmds.data();
  const uint8_t* end = p + cmds.size();
  for (uint32_t i = 0; i < h.ncmds && p + sizeof(struct load_command) <= end; i++) {
    auto* lc = reinterpret_cast<const struct load_command*>(p);
    if (lc->cmdsize == 0 || p + lc->cmdsize > end) break;

    if (lc->cmd == LC_SEGMENT_64) {
      auto* sc = reinterpret_cast<const struct segment_command_64*>(p);
      Segment seg;
      seg.name = std::string(sc->segname, strnlen(sc->segname, sizeof(sc->segname)));
      seg.vmaddr = sc->vmaddr; seg.vmsize = sc->vmsize;
      seg.fileoff = sc->fileoff; seg.filesize = sc->filesize;
      out.segments.push_back(seg);
      if (seg.name == "__TEXT") { textVmaddr = sc->vmaddr; out.textSize = sc->vmsize; haveText = true; }
      // セグメント内セクションを走査して __unwind_info（compact unwind）を記録（CFI 用）。
      const uint8_t* sp = p + sizeof(struct segment_command_64);
      for (uint32_t s = 0; s < sc->nsects && sp + sizeof(struct section_64) <= p + lc->cmdsize; s++) {
        auto* sec = reinterpret_cast<const struct section_64*>(sp);
        std::string sname(sec->sectname, strnlen(sec->sectname, sizeof(sec->sectname)));
        if (sname == "__unwind_info") { out.unwindVmaddr = sec->addr; out.unwindSize = sec->size; }
        else if (sname == "__eh_frame") { out.ehframeVmaddr = sec->addr; out.ehframeSize = sec->size; }
        else if (sname == "__stubs") {
          out.stubVmaddr = sec->addr; out.stubSize = sec->size;
          out.stubEntrySize = sec->reserved2; out.stubIndirectIndex = sec->reserved1;
        }
        sp += sizeof(struct section_64);
      }
    } else if (lc->cmd == LC_SYMTAB) {
      auto* st = reinterpret_cast<const struct symtab_command*>(p);
      out.hasSymtab = true;
      out.symoff = st->symoff; out.nsyms = st->nsyms; out.stroff = st->stroff; out.strsize = st->strsize;
    } else if (lc->cmd == LC_DYSYMTAB) {
      auto* dc = reinterpret_cast<const struct dysymtab_command*>(p);
      out.hasDysymtab = true;
      out.indirectsymoff = dc->indirectsymoff; out.nindirectsyms = dc->nindirectsyms;
    } else if (lc->cmd == LC_UUID) {
      auto* uc = reinterpret_cast<const struct uuid_command*>(p);
      out.hasUuid = true;
      memcpy(out.uuid, uc->uuid, 16);
    }
    p += lc->cmdsize;
  }

  out.slide = haveText ? (loadAddr - textVmaddr) : 0;
  return true;
}

const Image* ImageList::byAddress(uint64_t addr) const {
  // まず __TEXT 範囲
  for (const auto& img : images_) if (img.containsText(addr)) return &img;
  // 次に他セグメント（__LINKEDIT/__PAGEZERO は除く）
  for (const auto& img : images_) {
    for (const auto& s : img.segments) {
      if (s.name == "__LINKEDIT" || s.name == "__PAGEZERO") continue;
      uint64_t base = s.vmaddr + img.slide;
      if (s.vmsize && addr >= base && addr < base + s.vmsize) return &img;
    }
  }
  return nullptr;
}

const Image* ImageList::byName(const std::string& name) const {
  auto lower = [](std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
  std::string want = lower(name);
  for (const auto& img : images_) if (img.name == name) return &img; // 完全一致優先
  for (const auto& img : images_) {
    std::string n = lower(img.name);
    if (n == want || n.substr(0, n.find('.')) == want) return &img;
  }
  return nullptr;
}

const Image* ImageList::mainExecutable() const {
  for (const auto& img : images_) {
    struct mach_header_64 h = {};
    if (target_.readMemory(img.loadAddr, &h, sizeof(h)) && h.magic == MH_MAGIC_64 && h.filetype == MH_EXECUTE)
      return &img;
  }
  return nullptr;
}

} // namespace ta2
