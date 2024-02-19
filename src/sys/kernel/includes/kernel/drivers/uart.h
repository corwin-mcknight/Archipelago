#pragma once

#include <stdint.h>

#include "kernel/drivers/logging_device.h"

namespace kernel {
namespace driver {

class uart : public logging_device {
    constexpr static uint16_t port = 0x3f8;

   public:
    const char* name() const override;
    void init() override;
    void write_byte(char c) override;

   private:
    bool transmit_empty();
    int recieved_data();
    char read();
};

}  // namespace driver
}  // namespace kernel