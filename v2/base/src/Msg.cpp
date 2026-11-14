#include "base/Msg.h"

#include <cstddef>
#include <utility>

namespace ta2 {

const char* msgKey(MsgId id) {
  static const char* const kKeys[] = {
#define TA2_MSG(id, key, ja) key,
#include "base/Messages.def"
#undef TA2_MSG
  };
  size_t i = static_cast<size_t>(id);
  return i < sizeof(kKeys) / sizeof(kKeys[0]) ? kKeys[i] : "";
}

const MsgArg* Msg::find(const char* name) const {
  for (const MsgArg& a : args)
    if (a.name == name) return &a;
  return nullptr;
}

Msg Msg::of(MsgId id) {
  Msg m;
  m.id = id;
  return m;
}

Msg& Msg::hex(const char* name, uint64_t v) & {
  args.push_back(MsgArg{name, v, true});
  return *this;
}

Msg& Msg::num(const char* name, int64_t v) & {
  args.push_back(MsgArg{name, v, false});
  return *this;
}

Msg& Msg::str(const char* name, std::string v) & {
  args.push_back(MsgArg{name, std::move(v), false});
  return *this;
}

Msg& Msg::sub(const char* name, Msg m) & {
  args.push_back(MsgArg{name, std::make_shared<Msg>(std::move(m)), false});
  return *this;
}

} // namespace ta2
