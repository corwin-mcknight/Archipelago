#pragma once

namespace kernel::input {

/// Return one console-input byte, or -1 when every input source is idle.
/// Serial is preferred when both serial and a board-local keyboard have data.
int try_read_char();

}  // namespace kernel::input
