#pragma once

#include <kernel/obj/types.h>

#include <ktl/atomic>

#define DECLARE_OBJECT_TYPE(ClassName, TypeIdValue) static constexpr kernel::obj::TypeId TYPE_ID = TypeIdValue;

namespace kernel::obj {

class Object {
   public:
    explicit Object(TypeId type_id);
    virtual ~Object();

    ObjectId id() const { return m_id; }
    TypeId type_id() const { return m_type_id; }

    const char* name() const { return m_name; }
    void set_name(const char* name) { m_name = name; }

    uint32_t signals() const;
    void signal_set(uint32_t bits);
    void signal_clear(uint32_t bits);

   private:
    ObjectId m_id;
    TypeId m_type_id;
    const char* m_name = nullptr;
    ktl::atomic<uint32_t> m_signals{0};

    static ObjectId allocate_id();
};

}  // namespace kernel::obj
