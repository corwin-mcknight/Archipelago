#pragma once

#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>

#include <ktl/result>

namespace kernel::obj {

class Event : public Object {
   public:
    DECLARE_OBJECT_TYPE(Event, type_ids::EVENT)

    Event() : Object(TYPE_ID) {}

    static ktl::result<void> register_type(TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "event", RIGHT_READ | RIGHT_SIGNAL | RIGHT_DUPLICATE,
                                      RIGHT_READ | RIGHT_SIGNAL);
    }
};

}  // namespace kernel::obj
