#pragma once

#include <kernel/obj/types.h>

#include <ktl/string_view>

namespace kernel::obj {

struct TypeDescriptor {
    TypeId id;
    ktl::string_view name;
    Rights valid_rights;
};

}  // namespace kernel::obj
