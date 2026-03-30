#pragma once

#include <kernel/obj/types.h>

namespace kernel::obj {

struct TypeDescriptor {
    TypeId id;
    const char* name;
    Rights valid_rights;
    Rights default_rights;
};

}  // namespace kernel::obj
