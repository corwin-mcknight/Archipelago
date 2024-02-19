#pragma once

namespace kernel {
namespace driver {

class logging_device {
   public:
    virtual const char* name() const = 0;

    virtual void init() = 0;
    virtual void write_byte(char c) = 0;
};

}  // namespace driver
}  // namespace kernel