#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/string_view>

namespace kernel::boot { struct boot_info; }

namespace kernel {

class Console {
   public:
    Console()                          = default;
    Console(const Console&)            = delete;
    Console& operator=(const Console&) = delete;

    class LineGuard {
       public:
        explicit LineGuard(Console& console);
        ~LineGuard();
        LineGuard(const LineGuard&)            = delete;
        LineGuard& operator=(const LineGuard&) = delete;

       private:
        Console& m_console;
        uint64_t m_flags;
    };

    void write(char c);
    void write(ktl::string_view text);
    void init(const boot::boot_info& info);

    LineGuard lock_line() { return LineGuard(*this); }

   private:
    friend class LineGuard;

    static constexpr size_t NO_OWNER = SIZE_MAX;
    volatile size_t m_line_owner     = NO_OWNER;
    size_t m_line_depth              = 0;
};

extern Console g_console;

}  // namespace kernel
