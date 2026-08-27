#include <kernel/drivers/uart.h>
#include <kernel/input.h>
#include <kernel/platform.h>

extern kernel::driver::uart uart;

namespace kernel::input {

int try_read_char() {
    if (uart.received_data() != 0) { return static_cast<unsigned char>(uart.read()); }
    return kernel::platform::console_input_read();
}

}  // namespace kernel::input
