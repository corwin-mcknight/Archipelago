#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/console.h>
#include <kernel/data/font_unscii.h>
#include <kernel/drivers/uart.h>
#include <kernel/log.h>
#include <kernel/platform.h>
#include <kernel/sched/scheduler.h>
#include <kernel/synchronization/guard.h>
#include <kernel/synchronization/spinlock.h>
#include <std/new.h>
#include <std/string.h>

#include <ktl/algorithm>

extern kernel::driver::uart uart;

namespace kernel {
namespace {

using kernel::synchronization::critical_irq_lock_guard;
using kernel::synchronization::spinlock;

// The UART remains authoritative when the framebuffer queue overflows.
constexpr size_t RING_SIZE = 1u << 14;  // 16 KiB
constexpr size_t RING_MASK = RING_SIZE - 1;

[[clang::no_destroy]] spinlock g_lock;
char g_ring[RING_SIZE];
size_t g_head              = 0;
size_t g_tail              = 0;
uint64_t g_dropped         = 0;

// Release/acquire publishes the terminal state to writers.
bool g_ready               = false;

constexpr uint32_t GLYPH_W = 8, GLYPH_H = 8;
constexpr uint8_t DEFAULT_FG = 7;  // xterm light grey
constexpr uint8_t DEFAULT_BG = 0;  // black

struct Cell {
    char ch;
    uint8_t fg;
    uint8_t bg;

    bool operator==(const Cell&) const = default;
};

uint32_t g_palette[256];
uint32_t g_red_shift = 16, g_green_shift = 8, g_blue_shift = 0;

uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(r) << g_red_shift) | (static_cast<uint32_t>(g) << g_green_shift) |
           (static_cast<uint32_t>(b) << g_blue_shift);
}

void build_palette() {
    static const uint32_t base16[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
                                        0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};
    for (int i = 0; i < 16; ++i) {
        uint32_t v   = base16[i];
        g_palette[i] = pack_rgb((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    }
    static const uint8_t step[6] = {0, 95, 135, 175, 215, 255};
    for (int r = 0; r < 6; ++r) {
        for (int g = 0; g < 6; ++g) {
            for (int b = 0; b < 6; ++b) { g_palette[16 + 36 * r + 6 * g + b] = pack_rgb(step[r], step[g], step[b]); }
        }
    }
    for (int i = 0; i < 24; ++i) {
        uint8_t v          = static_cast<uint8_t>(8 + 10 * i);
        g_palette[232 + i] = pack_rgb(v, v, v);
    }
}

uint8_t rgb_to_256(int r, int g, int b) {
    auto q = [](int v) { return (v * 5 + 127) / 255; };  // 0..5
    return static_cast<uint8_t>(16 + 36 * q(r) + 6 * q(g) + q(b));
}

enum class ParserState { TEXT, ESCAPE, CSI };

struct Terminal {
    uint8_t* fb;
    uint64_t pitch;
    uint32_t cols, rows;
    uint32_t cx, cy;  // cursor, in cells
    Cell* grid;       // logical contents
    Cell* shadow;     // what is currently on the panel; the diff target
    bool dirty;       // grid changed since the last flush; gates the diff so an idle console is free
    bool hold;        // inside CSI ?2026 h..l (synchronized output): defer flushing until the frame ends

    uint8_t fg, bg;
    bool bold, reverse;

    ParserState parser_state;
    int params[16];
    int nparams;
    int cur;
    bool have_cur;
    int priv;  // private-marker byte (?, >, =) or 0
} t;

Cell blank_cell() { return Cell{' ', DEFAULT_FG, t.bg}; }

void push(char c) {  // caller holds g_lock
    if (g_head - g_tail >= RING_SIZE) {
        ++g_dropped;
        return;
    }
    g_ring[g_head & RING_MASK] = c;
    ++g_head;
}

size_t pop_batch(char* out, size_t cap) {
    critical_irq_lock_guard guard(g_lock);
    size_t n = 0;
    while (n < cap && g_tail != g_head) { out[n++] = g_ring[g_tail++ & RING_MASK]; }
    return n;
}

// The scanned-out framebuffer is write-only from the kernel's point of view.
void blit(const Cell& c, uint32_t gx, uint32_t gy) {
    auto uc = static_cast<uint8_t>(c.ch);
    if (uc < 0x20) { uc = 0x20; }
    const uint8_t* glyph = font_unscii8_bitmap[uc - 0x20];
    uint32_t fg          = g_palette[c.fg];
    uint32_t bg          = g_palette[c.bg];
    uint64_t x0          = static_cast<uint64_t>(gx) * GLYPH_W;
    uint64_t y0          = static_cast<uint64_t>(gy) * GLYPH_H;
    for (uint32_t row = 0; row < GLYPH_H; ++row) {
        auto* line   = reinterpret_cast<uint32_t*>(t.fb + (y0 + row) * t.pitch + x0 * 4);
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < GLYPH_W; ++col) { line[col] = (bits & (0x80 >> col)) ? fg : bg; }
    }
}

void flush() {
    for (uint32_t gy = 0; gy < t.rows; ++gy) {
        size_t base = static_cast<size_t>(gy) * t.cols;
        int lo = -1, hi = -1;
        for (uint32_t gx = 0; gx < t.cols; ++gx) {
            if (t.grid[base + gx] == t.shadow[base + gx]) { continue; }
            blit(t.grid[base + gx], gx, gy);
            t.shadow[base + gx] = t.grid[base + gx];
            if (lo < 0) { lo = static_cast<int>(gx); }
            hi = static_cast<int>(gx);
        }
        if (lo < 0) { continue; }
        uint64_t y0    = static_cast<uint64_t>(gy) * GLYPH_H;
        uint64_t x0    = static_cast<uint64_t>(lo) * GLYPH_W;
        uint64_t bytes = static_cast<uint64_t>(hi - lo + 1) * GLYPH_W * 4;
        for (uint32_t row = 0; row < GLYPH_H; ++row) {
            kernel::platform::dcache_clean_range(t.fb + (y0 + row) * t.pitch + x0 * 4, bytes);
        }
    }
}

void scroll() {
    size_t row_cells = t.cols;
    memmove(t.grid, t.grid + row_cells, static_cast<size_t>(t.rows - 1) * row_cells * sizeof(Cell));
    Cell* last = t.grid + static_cast<size_t>(t.rows - 1) * row_cells;
    for (uint32_t i = 0; i < t.cols; ++i) { last[i] = blank_cell(); }
    t.dirty = true;
}

void newline() {  // our producers emit '\n' as a line break, so treat it as CR+LF
    t.cx = 0;
    if (++t.cy >= t.rows) {
        scroll();
        t.cy = t.rows - 1;
    }
}

void put(char ch) {
    uint8_t fg = t.fg, bg = t.bg;
    if (t.bold && fg < 8) { fg += 8; }
    if (t.reverse) {
        uint8_t tmp = fg;
        fg          = bg;
        bg          = tmp;
    }
    t.grid[static_cast<size_t>(t.cy) * t.cols + t.cx] = Cell{ch, fg, bg};
    t.dirty                                           = true;
    if (++t.cx >= t.cols) { newline(); }
}

void erase_cells(size_t from, size_t to) {  // [from, to)
    for (size_t i = from; i < to; ++i) { t.grid[i] = blank_cell(); }
    t.dirty = true;
}

int param(int i, int def) { return i < t.nparams ? t.params[i] : def; }

void reset_pen() {
    t.fg      = DEFAULT_FG;
    t.bg      = DEFAULT_BG;
    t.bold    = false;
    t.reverse = false;
}

bool set_basic_color(int code) {
    if (code >= 30 && code <= 37) {
        t.fg = static_cast<uint8_t>(code - 30);
    } else if (code == 39) {
        t.fg = DEFAULT_FG;
    } else if (code >= 40 && code <= 47) {
        t.bg = static_cast<uint8_t>(code - 40);
    } else if (code == 49) {
        t.bg = DEFAULT_BG;
    } else if (code >= 90 && code <= 97) {
        t.fg = static_cast<uint8_t>(code - 90 + 8);
    } else if (code >= 100 && code <= 107) {
        t.bg = static_cast<uint8_t>(code - 100 + 8);
    } else {
        return false;
    }
    return true;
}

bool set_extended_color(int& index, int code) {
    if (code != 38 && code != 48) { return false; }
    uint8_t* color = code == 38 ? &t.fg : &t.bg;
    if (index + 2 < t.nparams && t.params[index + 1] == 5) {
        if (t.params[index + 2] < 0 || t.params[index + 2] > 255) { return false; }
        *color = static_cast<uint8_t>(t.params[index + 2]);
        index += 2;
        return true;
    }
    if (index + 4 < t.nparams && t.params[index + 1] == 2) {
        if (t.params[index + 2] < 0 || t.params[index + 2] > 255 || t.params[index + 3] < 0 ||
            t.params[index + 3] > 255 || t.params[index + 4] < 0 || t.params[index + 4] > 255) {
            return false;
        }
        *color = rgb_to_256(t.params[index + 2], t.params[index + 3], t.params[index + 4]);
        index += 4;
        return true;
    }
    return false;
}

void set_style(int code) {
    switch (code) {
        case 0: reset_pen(); break;
        case 1: t.bold = true; break;
        case 7: t.reverse = true; break;
        case 22: t.bold = false; break;
        case 27: t.reverse = false; break;
        default: break;
    }
}

void do_sgr() {
    if (t.nparams == 0) {
        reset_pen();
        return;
    }
    for (int i = 0; i < t.nparams; ++i) {
        int code = t.params[i];
        if (set_basic_color(code)) { continue; }
        if (set_extended_color(i, code)) { continue; }
        set_style(code);
    }
}

int distance_param(int index = 0) {
    int value = param(index, 1);
    return value == 0 ? 1 : value;
}

void set_cursor(int64_t row, int64_t column) {
    t.cy = static_cast<uint32_t>(ktl::clamp(row - 1, int64_t{0}, static_cast<int64_t>(t.rows) - 1));
    t.cx = static_cast<uint32_t>(ktl::clamp(column - 1, int64_t{0}, static_cast<int64_t>(t.cols) - 1));
}

void move_cursor(char command) {
    int distance = distance_param();
    switch (command) {
        case 'A': set_cursor(static_cast<int64_t>(t.cy) + 1 - distance, static_cast<int64_t>(t.cx) + 1); break;
        case 'B': set_cursor(static_cast<int64_t>(t.cy) + 1 + distance, static_cast<int64_t>(t.cx) + 1); break;
        case 'C': set_cursor(static_cast<int64_t>(t.cy) + 1, static_cast<int64_t>(t.cx) + 1 + distance); break;
        case 'D': set_cursor(static_cast<int64_t>(t.cy) + 1, static_cast<int64_t>(t.cx) + 1 - distance); break;
        case 'G': set_cursor(static_cast<int64_t>(t.cy) + 1, distance); break;
        case 'd': set_cursor(distance, static_cast<int64_t>(t.cx) + 1); break;
        default: break;
    }
}

void erase_display() {
    size_t total  = static_cast<size_t>(t.rows) * t.cols;
    size_t cursor = static_cast<size_t>(t.cy) * t.cols + t.cx;
    switch (param(0, 0)) {
        case 0: erase_cells(cursor, total); break;
        case 1: erase_cells(0, cursor + 1); break;
        default: erase_cells(0, total); break;
    }
}

void erase_line() {
    size_t row = static_cast<size_t>(t.cy) * t.cols;
    switch (param(0, 0)) {
        case 0: erase_cells(row + t.cx, row + t.cols); break;
        case 1: erase_cells(row, row + t.cx + 1); break;
        default: erase_cells(row, row + t.cols); break;
    }
}

void dispatch_private(char command) {
    if (t.priv != '?' || param(0, 0) != 2026) { return; }
    if (command == 'h') { t.hold = true; }
    if (command == 'l') { t.hold = false; }
}

void dispatch(char command) {
    if (t.priv != 0) {
        dispatch_private(command);
        return;
    }
    switch (command) {
        case 'H':
        case 'f': set_cursor(distance_param(0), distance_param(1)); break;
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'G':
        case 'd': move_cursor(command); break;
        case 'J': erase_display(); break;
        case 'K': erase_line(); break;
        case 'm': do_sgr(); break;
        default: break;
    }
}

void csi_byte(char c) {
    auto uc = static_cast<uint8_t>(c);
    if (t.nparams == 0 && !t.have_cur && uc >= 0x3c && uc <= 0x3f) {  // ? > = < private marker
        t.priv = uc;
        return;
    }
    if (c >= '0' && c <= '9') {
        int digit  = c - '0';
        t.cur      = t.cur > (INT32_MAX - digit) / 10 ? INT32_MAX : t.cur * 10 + digit;
        t.have_cur = true;
        return;
    }
    if (c == ';') {
        if (t.nparams < 16) { t.params[t.nparams++] = t.cur; }
        t.cur      = 0;
        t.have_cur = false;
        return;
    }
    if (uc >= 0x40 && uc <= 0x7e) {  // final byte
        if (t.have_cur && t.nparams < 16) { t.params[t.nparams++] = t.cur; }
        dispatch(c);
        t.parser_state = ParserState::TEXT;
        return;
    }
}

void begin_csi() {
    t.parser_state = ParserState::CSI;
    t.nparams      = 0;
    t.cur          = 0;
    t.have_cur     = false;
    t.priv         = 0;
}

void render_text(char c) {
    switch (c) {
        case 0x1b: t.parser_state = ParserState::ESCAPE; break;
        case '\n': newline(); break;
        case '\r': t.cx = 0; break;
        case '\b':
            if (t.cx > 0) { --t.cx; }
            break;
        case '\t': {
            uint32_t next = (t.cx & ~7u) + 8;
            while (t.cx < next && t.cx < t.cols) { put(' '); }
            break;
        }
        default:
            if (static_cast<uint8_t>(c) >= 0x20) { put(c); }
            break;
    }
}

void render(char c) {
    switch (t.parser_state) {
        case ParserState::TEXT: render_text(c); break;
        case ParserState::ESCAPE:
            if (c == '[') {
                begin_csi();
            } else {
                t.parser_state = ParserState::TEXT;
            }
            break;
        case ParserState::CSI: csi_byte(c); break;
    }
}

constexpr size_t FLUSH_BYTES   = 1u << 16;

constexpr uint64_t FRAME_TICKS = 16;
constexpr uint64_t IDLE_TICKS  = 8;

[[noreturn]] void painter_thread(void*) {
    char batch[256];
    size_t since_flush = 0;
    while (true) {
        size_t n = pop_batch(batch, sizeof(batch));
        if (n > 0) {
            for (size_t i = 0; i < n; ++i) { render(batch[i]); }
            since_flush += n;
        }
        bool hold = t.hold && since_flush < FLUSH_BYTES;
        if (t.dirty && !hold && (n < sizeof(batch) || since_flush >= FLUSH_BYTES)) {
            flush();
            t.dirty     = false;
            since_flush = 0;
            kernel::sched::sleep_ticks(FRAME_TICKS);
        } else if (n == 0) {
            kernel::sched::sleep_ticks(IDLE_TICKS);
        }
    }
}

}  // namespace

Console g_console;

Console::LineGuard::LineGuard(Console& console)
    : m_console(console), m_flags(kernel::arch::save_and_disable_interrupts()) {
    size_t self = kernel::arch::current_core_index();
    if (__atomic_load_n(&m_console.m_line_owner, __ATOMIC_RELAXED) == self) {
        ++m_console.m_line_depth;
        return;
    }
    size_t expected = Console::NO_OWNER;
    while (!__atomic_compare_exchange_n(&m_console.m_line_owner, &expected, self, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
        expected = Console::NO_OWNER;
    }
    m_console.m_line_depth = 1;
}

Console::LineGuard::~LineGuard() {
    if (--m_console.m_line_depth == 0) {
        __atomic_store_n(&m_console.m_line_owner, Console::NO_OWNER, __ATOMIC_RELEASE);
    }
    kernel::arch::restore_interrupts(m_flags);
}

void Console::write(char c) {
    uart.write_byte(c);
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) { return; }
    critical_irq_lock_guard guard(g_lock);
    push(c);
}

void Console::write(ktl::string_view text) {
    uart.write_string(text);
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) { return; }
    critical_irq_lock_guard guard(g_lock);
    for (char c : text) { push(c); }
}

void Console::init(const boot::boot_info& info) {
    if (info.framebuffer == nullptr || info.fb_bpp != 32) { return; }

    // Treat the bootloader descriptor as untrusted input. In particular, a
    // zero/short pitch aliases scanlines onto the same MMIO addresses, which
    // can wedge real display hardware instead of merely drawing incorrectly.
    constexpr uint64_t BYTES_PER_PIXEL = 4;
    if (info.fb_width == 0 || info.fb_height == 0 || info.fb_width > UINT64_MAX / BYTES_PER_PIXEL ||
        info.fb_pitch < info.fb_width * BYTES_PER_PIXEL || info.fb_height > SIZE_MAX / info.fb_pitch) {
        g_log.warn("console: ignoring invalid framebuffer geometry");
        return;
    }

    // pack_rgb() shifts an eight-bit channel inside a uint32_t. A larger shift is
    // undefined, and would also place part of the channel beyond the pixel.
    if (info.fb_red_shift > 24 || info.fb_green_shift > 24 || info.fb_blue_shift > 24) {
        g_log.warn("console: ignoring invalid framebuffer colour layout");
        return;
    }

    uint64_t cols64 = info.fb_width / GLYPH_W;
    uint64_t rows64 = info.fb_height / GLYPH_H;
    if (cols64 == 0 || rows64 == 0 || cols64 > INT32_MAX || rows64 > INT32_MAX) {
        g_log.warn("console: ignoring unrepresentable framebuffer grid");
        return;
    }

    uint32_t cols  = static_cast<uint32_t>(cols64);
    uint32_t rows  = static_cast<uint32_t>(rows64);

    size_t n_cells = static_cast<size_t>(cols) * rows;
    Cell* grid     = new (std::nothrow) Cell[n_cells];
    Cell* shadow   = new (std::nothrow) Cell[n_cells];
    if (grid == nullptr || shadow == nullptr) {
        delete[] grid;
        delete[] shadow;
        g_log.warn("console: framebuffer grid allocation failed; panel stays dark, UART only");
        return;
    }

    t.fb    = static_cast<uint8_t*>(info.framebuffer);
    t.pitch = info.fb_pitch;
    t.cols  = cols;
    t.rows  = rows;
    t.cx = t.cy = 0;
    t.grid      = grid;
    t.shadow    = shadow;
    t.fg        = DEFAULT_FG;
    t.bg        = DEFAULT_BG;
    t.bold = t.reverse = t.hold = false;
    t.parser_state              = ParserState::TEXT;
    t.nparams = t.cur = t.priv = 0;
    t.have_cur                 = false;

    g_red_shift                = info.fb_red_shift;
    g_green_shift              = info.fb_green_shift;
    g_blue_shift               = info.fb_blue_shift;
    build_palette();

    for (size_t i = 0; i < n_cells; ++i) { grid[i] = shadow[i] = Cell{' ', DEFAULT_FG, DEFAULT_BG}; }
    memset(t.fb, 0, t.pitch * info.fb_height);
    kernel::platform::dcache_clean_range(t.fb, t.pitch * info.fb_height);

    kernel::sched::spawn("fb_sw_log", painter_thread, nullptr).expect("console: framebuffer painter spawn failed");
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
}

}  // namespace kernel
