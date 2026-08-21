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

// Text-output fan-out plus a small framebuffer terminal. Writers (the log, the
// shell) call write_byte/write_string; those hit the boot UART inline and, once
// a framebuffer exists, drop the bytes into a ring the fb_sw_log thread drains.
//
// The painter keeps the console as a character grid in RAM and interprets a
// VT-ish escape stream (cursor positioning, erase, SGR colour incl. xterm
// 256-colour), so a full-screen TUI repaints in place instead of scrolling. On
// each drained burst it diffs the grid against a shadow of what is already on the
// panel and blits only the cells that changed, then writes those rows back out of
// cache (the display scans DRAM directly). Typing and in-place TUI updates touch a
// handful of cells; only genuine scrolling costs a whole frame.

extern kernel::driver::uart uart;

namespace kernel::console {
namespace {

using kernel::synchronization::critical_irq_lock_guard;
using kernel::synchronization::spinlock;

// --- byte ring: writers -> painter -----------------------------------------
// Free-running head/tail counters (their difference is the fill level); when it
// fills, the painter has fallen behind and the newest bytes are dropped -- the
// UART holds the authoritative copy, so the panel merely misses a little.
constexpr size_t RING_SIZE = 1u << 14;  // 16 KiB
constexpr size_t RING_MASK = RING_SIZE - 1;

spinlock g_lock;
char g_ring[RING_SIZE];
size_t g_head              = 0;
size_t g_tail              = 0;
uint64_t g_dropped         = 0;

// Set once init() has published the geometry and started the painter; until then
// write_* only touch the UART. Release/acquire so a writer that sees it true also
// sees the geometry.
bool g_ready               = false;

// --- glyph + colour ---------------------------------------------------------
constexpr uint32_t GLYPH_W = 8, GLYPH_H = 8;
constexpr uint8_t DEFAULT_FG = 7;  // xterm light grey
constexpr uint8_t DEFAULT_BG = 0;  // black

// One screen cell: a codepoint and its 256-colour palette indices. Packed to 3
// bytes so cells[] and the shadow together are ~200 KB at 1080p.
struct cell {
    char ch;
    uint8_t fg;
    uint8_t bg;
};
bool operator==(const cell& a, const cell& b) { return a.ch == b.ch && a.fg == b.fg && a.bg == b.bg; }

// The xterm 256-colour palette, resolved to this framebuffer's pixel layout at
// init(). 0-15 base ANSI, 16-231 a 6x6x6 cube, 232-255 a grey ramp.
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

// Nearest 6x6x6-cube index for a truecolour (38;2) request -- graceful degrade.
uint8_t rgb_to_256(int r, int g, int b) {
    auto q = [](int v) { return (v * 5 + 127) / 255; };  // 0..5
    return static_cast<uint8_t>(16 + 36 * q(r) + 6 * q(g) + q(b));
}

// --- terminal state (painter thread only) ----------------------------------
struct term {
    uint8_t* fb;
    uint64_t width, height, pitch;
    uint32_t cols, rows;
    uint32_t cx, cy;  // cursor, in cells
    cell* grid;       // logical contents
    cell* shadow;     // what is currently on the panel; the diff target
    bool dirty;       // grid changed since the last flush; gates the diff so an idle console is free
    bool hold;        // inside CSI ?2026 h..l (synchronized output): defer flushing until the frame ends

    // pen
    uint8_t fg, bg;
    bool bold, reverse;

    // CSI parser
    int esc;  // 0 normal, 1 after ESC, 2 inside CSI
    int params[16];
    int nparams;
    int cur;
    bool have_cur;
    int priv;  // private-marker byte (?, >, =) or 0
} t;

cell blank_cell() { return cell{' ', DEFAULT_FG, t.bg}; }

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// --- byte ring helpers ------------------------------------------------------
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

// --- rendering --------------------------------------------------------------
// Paint one cell to the framebuffer from its palette colours. Framebuffer writes
// only, never a read.
void blit(const cell& c, uint32_t gx, uint32_t gy) {
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

// Diff the grid against the shadow and repaint only what changed, writing each
// touched row's changed span back out of cache for the display controller.
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
    memmove(t.grid, t.grid + row_cells, static_cast<size_t>(t.rows - 1) * row_cells * sizeof(cell));
    cell* last = t.grid + static_cast<size_t>(t.rows - 1) * row_cells;
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
    t.grid[static_cast<size_t>(t.cy) * t.cols + t.cx] = cell{ch, fg, bg};
    t.dirty                                           = true;
    if (++t.cx >= t.cols) { newline(); }
}

void erase_cells(size_t from, size_t to) {  // [from, to)
    for (size_t i = from; i < to; ++i) { t.grid[i] = blank_cell(); }
    t.dirty = true;
}

int param(int i, int def) { return i < t.nparams ? t.params[i] : def; }

void do_sgr() {
    if (t.nparams == 0) {  // bare CSI m == reset
        t.fg = DEFAULT_FG, t.bg = DEFAULT_BG, t.bold = false, t.reverse = false;
        return;
    }
    for (int i = 0; i < t.nparams; ++i) {
        int n = t.params[i];
        if (n == 0) {
            t.fg = DEFAULT_FG, t.bg = DEFAULT_BG, t.bold = false, t.reverse = false;
        } else if (n == 1) {
            t.bold = true;
        } else if (n == 22) {
            t.bold = false;
        } else if (n == 7) {
            t.reverse = true;
        } else if (n == 27) {
            t.reverse = false;
        } else if (n >= 30 && n <= 37) {
            t.fg = static_cast<uint8_t>(n - 30);
        } else if (n == 39) {
            t.fg = DEFAULT_FG;
        } else if (n >= 40 && n <= 47) {
            t.bg = static_cast<uint8_t>(n - 40);
        } else if (n == 49) {
            t.bg = DEFAULT_BG;
        } else if (n >= 90 && n <= 97) {
            t.fg = static_cast<uint8_t>(n - 90 + 8);
        } else if (n >= 100 && n <= 107) {
            t.bg = static_cast<uint8_t>(n - 100 + 8);
        } else if ((n == 38 || n == 48) && i + 2 < t.nparams && t.params[i + 1] == 5) {
            uint8_t idx             = static_cast<uint8_t>(t.params[i + 2]);
            (n == 38 ? t.fg : t.bg) = idx;
            i += 2;
        } else if ((n == 38 || n == 48) && i + 4 < t.nparams && t.params[i + 1] == 2) {
            uint8_t idx             = rgb_to_256(t.params[i + 2], t.params[i + 3], t.params[i + 4]);
            (n == 38 ? t.fg : t.bg) = idx;
            i += 4;
        }
    }
}

void dispatch(char final) {
    if (t.priv != 0) {  // DEC private modes: only synchronized output (?2026) matters to us; swallow the rest
        if (t.priv == '?' && param(0, 0) == 2026 && (final == 'h' || final == 'l')) { t.hold = (final == 'h'); }
        return;
    }
    switch (final) {
        case 'H':
        case 'f': {
            int r = param(0, 1);
            int c = param(1, 1);
            t.cy  = static_cast<uint32_t>(clampi((r ? r : 1) - 1, 0, static_cast<int>(t.rows) - 1));
            t.cx  = static_cast<uint32_t>(clampi((c ? c : 1) - 1, 0, static_cast<int>(t.cols) - 1));
            break;
        }
        case 'A':
            t.cy = static_cast<uint32_t>(
                clampi(static_cast<int>(t.cy) - (param(0, 1) ? param(0, 1) : 1), 0, static_cast<int>(t.rows) - 1));
            break;
        case 'B':
            t.cy = static_cast<uint32_t>(
                clampi(static_cast<int>(t.cy) + (param(0, 1) ? param(0, 1) : 1), 0, static_cast<int>(t.rows) - 1));
            break;
        case 'C':
            t.cx = static_cast<uint32_t>(
                clampi(static_cast<int>(t.cx) + (param(0, 1) ? param(0, 1) : 1), 0, static_cast<int>(t.cols) - 1));
            break;
        case 'D':
            t.cx = static_cast<uint32_t>(
                clampi(static_cast<int>(t.cx) - (param(0, 1) ? param(0, 1) : 1), 0, static_cast<int>(t.cols) - 1));
            break;
        case 'G':
            t.cx = static_cast<uint32_t>(clampi((param(0, 1) ? param(0, 1) : 1) - 1, 0, static_cast<int>(t.cols) - 1));
            break;
        case 'd':
            t.cy = static_cast<uint32_t>(clampi((param(0, 1) ? param(0, 1) : 1) - 1, 0, static_cast<int>(t.rows) - 1));
            break;
        case 'J': {
            size_t total  = static_cast<size_t>(t.rows) * t.cols;
            size_t cursor = static_cast<size_t>(t.cy) * t.cols + t.cx;
            int mode      = param(0, 0);
            if (mode == 0) {
                erase_cells(cursor, total);
            } else if (mode == 1) {
                erase_cells(0, cursor + 1);
            } else {
                erase_cells(0, total);
            }
            break;
        }
        case 'K': {
            size_t row = static_cast<size_t>(t.cy) * t.cols;
            int mode   = param(0, 0);
            if (mode == 0) {
                erase_cells(row + t.cx, row + t.cols);
            } else if (mode == 1) {
                erase_cells(row, row + t.cx + 1);
            } else {
                erase_cells(row, row + t.cols);
            }
            break;
        }
        case 'm': do_sgr(); break;
        default: break;  // unhandled CSI -- ignore
    }
}

void csi_byte(char c) {
    auto uc = static_cast<uint8_t>(c);
    if (t.nparams == 0 && !t.have_cur && uc >= 0x3c && uc <= 0x3f) {  // ? > = < private marker
        t.priv = uc;
        return;
    }
    if (c >= '0' && c <= '9') {
        t.cur      = t.cur * 10 + (c - '0');
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
        t.esc = 0;
        return;
    }
    // intermediate/space bytes: stay in CSI, ignore
}

void render(char c) {
    if (t.esc == 1) {
        if (c == '[') {
            t.esc      = 2;
            t.nparams  = 0;
            t.cur      = 0;
            t.have_cur = false;
            t.priv     = 0;
        } else {
            t.esc = 0;  // non-CSI escape (e.g. ESC(B) -- ignore its final byte too, cheaply
        }
        return;
    }
    if (t.esc == 2) {
        csi_byte(c);
        return;
    }
    if (c == 0x1b) {
        t.esc = 1;
        return;
    }
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        t.cx = 0;
        return;
    }
    if (c == '\b') {  // the shell erases with "\b \b"; the space overwrites between the two moves
        if (t.cx > 0) { --t.cx; }
        return;
    }
    if (c == '\t') {
        uint32_t next = (t.cx & ~7u) + 8;
        while (t.cx < next && t.cx < t.cols) { put(' '); }
        return;
    }
    if (static_cast<uint8_t>(c) < 0x20) { return; }
    put(c);
}

// Only flush mid-drain once this many bytes have piled up, so a whole TUI frame
// (a screen repaint between cursor-home and the next idle) lands in one flush at
// the natural frame boundary -- the ring draining -- rather than tearing across
// two. Purely a safety valve for an unbounded flood; interactive bursts drain
// and flush well below it.
constexpr size_t FLUSH_BYTES   = 1u << 16;

// Cap the repaint rate at ~60 Hz: after a flush, hold this many ticks (1 tick = 1 ms)
// before the next one, so a chatty producer cannot drive the painter's CPU up. When
// idle, poll a little slower still. ponytail: a wait-queue wake on push would drop
// idle CPU to zero -- do that if this poll ever shows up in a profile.
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
        // Flush when the ring drains (n < batch) or a flood crosses the safety cap, but only if
        // something changed -- gating on the dirty flag keeps an idle console off the CPU entirely
        // (no full-grid diff when nothing was written).
        bool hold = t.hold && since_flush < FLUSH_BYTES;  // the flood cap also bounds a frame left open
        if (t.dirty && !hold && (n < sizeof(batch) || since_flush >= FLUSH_BYTES)) {
            flush();
            t.dirty     = false;
            since_flush = 0;
            kernel::sched::sleep_ticks(FRAME_TICKS);
        } else if (n == 0) {
            kernel::sched::sleep_ticks(IDLE_TICKS);
        }
        // else: ring still had a full batch -- keep draining without sleeping.
    }
}

}  // namespace

void write_byte(char c) {
    uart.write_byte(c);
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) { return; }
    critical_irq_lock_guard guard(g_lock);
    push(c);
}

void write_string(ktl::string_view s) {
    uart.write_string(s);
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) { return; }
    critical_irq_lock_guard guard(g_lock);
    for (char c : s) { push(c); }
}

void init(const kernel::boot::boot_info& info) {
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

    uint32_t cols = static_cast<uint32_t>(info.fb_width / GLYPH_W);
    uint32_t rows = static_cast<uint32_t>(info.fb_height / GLYPH_H);
    if (cols == 0 || rows == 0) { return; }

    size_t n_cells = static_cast<size_t>(cols) * rows;
    cell* grid     = new (std::nothrow) cell[n_cells];
    cell* shadow   = new (std::nothrow) cell[n_cells];
    if (grid == nullptr || shadow == nullptr) {
        delete[] grid;
        delete[] shadow;
        g_log.warn("console: framebuffer grid allocation failed; panel stays dark, UART only");
        return;
    }

    t.fb     = static_cast<uint8_t*>(info.framebuffer);
    t.width  = info.fb_width;
    t.height = info.fb_height;
    t.pitch  = info.fb_pitch;
    t.cols   = cols;
    t.rows   = rows;
    t.cx = t.cy = 0;
    t.grid      = grid;
    t.shadow    = shadow;
    t.fg        = DEFAULT_FG;
    t.bg        = DEFAULT_BG;
    t.bold = t.reverse = t.hold = false;
    t.esc = t.nparams = t.cur = t.priv = 0;
    t.have_cur                         = false;

    g_red_shift                        = info.fb_red_shift;
    g_green_shift                      = info.fb_green_shift;
    g_blue_shift                       = info.fb_blue_shift;
    build_palette();

    // grid and shadow both start blank-on-black; clear the panel to match so the
    // first diff draws only real content.
    for (size_t i = 0; i < n_cells; ++i) { grid[i] = shadow[i] = cell{' ', DEFAULT_FG, DEFAULT_BG}; }
    memset(t.fb, 0, t.pitch * t.height);
    kernel::platform::dcache_clean_range(t.fb, t.pitch * t.height);

    kernel::sched::spawn("fb_sw_log", painter_thread, nullptr).expect("console: framebuffer painter spawn failed");
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
}

}  // namespace kernel::console
