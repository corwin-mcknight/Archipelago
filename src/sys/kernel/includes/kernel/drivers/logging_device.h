#pragma once

#include "ktl/string_view"
namespace kernel {
namespace driver {

class logging_device {
   public:
    virtual const char* name() const = 0;

    virtual void init() = 0;
    virtual void write_byte(char c) = 0;
    virtual void write_string(ktl::string_view s) {
        for (size_t i = 0; i < s.size(); i++) { write_byte(s[i]); }
    }
};

}  // namespace driver
}  // namespace kernel