#pragma once

#include "ktl/string_view"
namespace kernel {
namespace driver {

class logging_device {
   public:
    virtual const char* name() const = 0;
    virtual ~logging_device()        = default;

    virtual void init()              = 0;
    virtual void write_byte(char c)  = 0;
    virtual void write_string(ktl::string_view s) {
        for (char c : s) { write_byte(c); }
    }
};

}  // namespace driver
}  // namespace kernel