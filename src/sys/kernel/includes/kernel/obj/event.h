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

    static Result<bool, result_t> register_type(TypeRegistry& registry) {
        auto result = registry.register_type(TYPE_ID, "event", RIGHT_READ | RIGHT_SIGNAL | RIGHT_DUPLICATE,
                                             RIGHT_READ | RIGHT_SIGNAL);
        if (result.is_err()) { return Result<bool, result_t>::err(result.unwrap_err()); }
        return Result<bool, result_t>::ok(true);
    }
};

}  // namespace kernel::obj
