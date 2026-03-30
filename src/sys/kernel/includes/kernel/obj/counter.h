#pragma once

#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>

#include <ktl/atomic>
#include <ktl/result>

namespace kernel::obj {

class Counter : public Object {
   public:
    DECLARE_OBJECT_TYPE(Counter, type_ids::COUNTER)

    explicit Counter(uint64_t initial = 0) : Object(TYPE_ID), m_value(initial) {}

    uint64_t value() const { return m_value.load(ktl::memory_order::acquire); }
    uint64_t increment(uint64_t amount = 1) { return m_value.fetch_add(amount, ktl::memory_order::seq_cst); }
    void reset() { m_value.store(0, ktl::memory_order::release); }

    static Result<bool, result_t> register_type(TypeRegistry& registry) {
        auto result =
            registry.register_type(TYPE_ID, "counter", RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE, RIGHT_READ);
        if (result.is_err()) { return Result<bool, result_t>::err(result.unwrap_err()); }
        return Result<bool, result_t>::ok(true);
    }

   private:
    ktl::atomic<uint64_t> m_value;
};

}  // namespace kernel::obj
