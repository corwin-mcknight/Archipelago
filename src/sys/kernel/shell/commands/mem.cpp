#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/config.h>
#include <kernel/drivers/uart.h>
#include <kernel/mm/early_heap.h>
#include <kernel/mm/page_descriptor.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/region.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/obj/type_registry.h>
#include <kernel/sched/scheduler.h>
#include <kernel/shell/output.h>

#include <ktl/fmt>
#include <ktl/string_view>

extern kernel::driver::uart uart;

namespace {

using namespace kernel::mm;
using kernel::shell::ShellOutput;

constexpr size_t PAGE_SIZE     = KERNEL_MINIMUM_PAGE_SIZE;

// SGR color codes.
constexpr const char* C_GREEN  = "32";
constexpr const char* C_YELLOW = "33";
constexpr const char* C_RED    = "31";
constexpr const char* C_CYAN   = "36";
constexpr const char* C_BLUE   = "34";
constexpr const char* C_MAG    = "35";
constexpr const char* C_DIM    = "2";
constexpr const char* C_TITLE  = "1;36";

constexpr int BAR_WIDTH        = 26;
constexpr int STRIP_CELLS      = 64;

// Bars, strips, maps, and box-drawing write raw bytes, which would splice
// into the harness JSON stream; protocol mode gets the plain field lines
// only, everything through print().
bool visual(ShellOutput& out) { return !out.protocol_mode(); }

const char* util_color(uint64_t used, uint64_t total) {
    if (total == 0) { return C_GREEN; }
    uint64_t pct = used * 100 / total;
    if (pct < 75) { return C_GREEN; }
    if (pct < 90) { return C_YELLOW; }
    return C_RED;
}

// "12.3 MiB"-style rendering into buf; returns buf for inline use.
const char* human_bytes(char* buf, size_t n, uint64_t bytes) {
    constexpr uint64_t KIB = 1024, MIB = KIB * 1024, GIB = MIB * 1024;
    uint64_t unit    = 1;
    const char* name = "B";
    if (bytes >= GIB) {
        unit = GIB;
        name = "GiB";
    } else if (bytes >= MIB) {
        unit = MIB;
        name = "MiB";
    } else if (bytes >= KIB) {
        unit = KIB;
        name = "KiB";
    }
    uint64_t tenths = bytes * 10 / unit;
    if (tenths % 10 == 0 || tenths / 10 >= 100) {
        ktl::format::format_to_buffer_raw(buf, n, "{0} {1}", tenths / 10, name);
    } else {
        ktl::format::format_to_buffer_raw(buf, n, "{0}.{1} {2}", tenths / 10, tenths % 10, name);
    }
    return buf;
}

const char* human_pages(char* buf, size_t n, uint64_t pages) { return human_bytes(buf, n, pages * PAGE_SIZE); }

void section(ShellOutput& out, const char* title) {
    if (!visual(out)) {
        out.print("== {0}\n", title);
        return;
    }
    out.sgr(C_DIM);
    out.write("── ");
    out.sgr(C_TITLE);
    out.write(title);
    out.reset_style();
    out.write(" ");
    out.sgr(C_DIM);
    // Pad the rule out to a fixed visual width.
    size_t used = 4 + ktl::string_view(title).size();
    for (size_t i = used; i < 60; ++i) { out.write("─"); }
    out.reset_style();
    out.write("\n");
}

// [██████████░░░░░] usage meter, colored by utilization. Interactive only.
void draw_bar(ShellOutput& out, uint64_t used, uint64_t total) {
    int filled = total > 0 ? static_cast<int>(used * BAR_WIDTH / total) : 0;
    if (filled > BAR_WIDTH) { filled = BAR_WIDTH; }
    out.write("  [");
    out.sgr(util_color(used, total));
    for (int i = 0; i < filled; ++i) { out.write("█"); }
    out.sgr(C_DIM);
    for (int i = filled; i < BAR_WIDTH; ++i) { out.write("░"); }
    out.reset_style();
    out.write("]");
}

// ── physical ──────────────────────────────────────────────

void draw_phys_strip(ShellOutput& out) {
    if (!g_page_descriptors.initialized()) { return; }
    size_t total = g_page_descriptors.coverage_end() / PAGE_SIZE;
    if (total == 0) { return; }
    // Zoom the axis to RAM: skip the leading MMIO run (riscv64 DRAM sits at
    // 2 GiB, so plotting from address zero would waste most of the strip).
    size_t first = 0;
    while (first < total) {
        const page_descriptor* d = g_page_descriptors.lookup(first * PAGE_SIZE);
        if (d != nullptr && d->state != page_state::MMIO) { break; }
        ++first;
    }
    size_t span = total - first;
    if (span == 0) { return; }
    // Per-cell counts: [0]=free/zeroed, [1]=active/inactive, [2]=wired, [3]=mmio holes.
    uint32_t cells[STRIP_CELLS][4] = {};
    for (size_t pfn = first; pfn < total; ++pfn) {
        const page_descriptor* d = g_page_descriptors.lookup(pfn * PAGE_SIZE);
        if (d == nullptr) { continue; }
        size_t cell = (pfn - first) * STRIP_CELLS / span;
        switch (d->state) {
            case page_state::FREE:
            case page_state::ZEROED: ++cells[cell][0]; break;
            case page_state::ACTIVE:
            case page_state::INACTIVE: ++cells[cell][1]; break;
            case page_state::MMIO: ++cells[cell][3]; break;
            case page_state::WIRED:
            default: ++cells[cell][2]; break;
        }
    }
    out.write("  ");
    for (auto& cell : cells) {
        uint32_t free_ct = cell[0], used_ct = cell[1], wired = cell[2], mmio = cell[3];
        uint32_t sum = free_ct + used_ct + wired + mmio;
        if (sum == 0) {
            out.write(" ");
            continue;
        }
        // Holes between RAM regions render dim so memory stands out.
        if (mmio * 2 >= sum) {
            out.sgr(C_DIM);
            out.write("·");
            out.reset_style();
            continue;
        }
        const char* color = C_GREEN;
        uint32_t busy     = used_ct + wired;
        if (wired * 2 >= sum) {
            color = C_RED;
        } else if (used_ct * 2 >= sum) {
            color = C_YELLOW;
        }
        out.sgr(color);
        if (busy * 4 < sum) {
            out.write("░");
        } else if (busy * 2 < sum) {
            out.write("▒");
        } else if (busy * 4 < sum * 3) {
            out.write("▓");
        } else {
            out.write("█");
        }
        out.reset_style();
    }
    out.write("\n  ");
    out.sgr(C_DIM);
    char lo[24], hi[24];
    out.print("{0} .. {1} phys   ", human_bytes(lo, sizeof(lo), first * PAGE_SIZE),
              human_bytes(hi, sizeof(hi), g_page_descriptors.coverage_end()));
    out.sgr(C_GREEN);
    out.write("█");
    out.reset_style();
    out.write(" free ");
    out.sgr(C_YELLOW);
    out.write("█");
    out.reset_style();
    out.write(" used ");
    out.sgr(C_RED);
    out.write("█");
    out.reset_style();
    out.write(" wired ");
    out.sgr(C_DIM);
    out.write("· mmio/hole");
    out.reset_style();
    out.write("\n");
}

void render_phys(ShellOutput& out) {
    section(out, "physical");
    auto s = kernel::mm::g_page_frame_allocator.stats();
    char a[24], b[24], c[24];
    size_t used = s.total_pages - s.free_pages - s.reserved_pages;
    if (visual(out)) { draw_bar(out, used, s.total_pages); }
    out.print("  {0} / {1} free ({2}%)\n", human_pages(a, sizeof(a), s.free_pages),
              human_pages(b, sizeof(b), s.total_pages), s.total_pages > 0 ? s.free_pages * 100 / s.total_pages : 0);
    out.print("  zeroed {0} pg (pool {1} + tail {2})   dirty {3} pg   reserved {4}\n",
              s.zeroed_pooled + s.zeroed_region_tail, s.zeroed_pooled, s.zeroed_region_tail, s.dirty,
              human_pages(c, sizeof(c), s.reserved_pages));
    out.print("  allocs {0}   frees {1}   contig {2}   failures {3}   zeroer {4} pg   low-water {5}\n", s.alloc_count,
              s.free_count, s.contig_count, s.alloc_failures, s.zeroer_pages, human_pages(a, sizeof(a), s.low_water));
    if (s.regions > 0) {
        out.sgr(C_DIM);
        out.print("  {0:-4} {1:-14} {2:-10} {3}\n", "RGN", "BASE", "PAGES", "ZEROED-TAIL");
        out.reset_style();
        size_t idx = 0;
        kernel::mm::g_page_frame_allocator.for_each_region([&](const vm_page_region& r) {
            char base[20];
            ktl::format::format_to_buffer_raw(base, sizeof(base), "0x{0:p}", r.start);
            out.print("  {0:-4} {1:-14} {2:-10} {3}\n", idx++, static_cast<const char*>(base), r.count, r.zeroed_count);
        });
    }
    if (visual(out)) { draw_phys_strip(out); }
}

// ── pages ─────────────────────────────────────────────────

void render_pages(ShellOutput& out) {
    section(out, "pages");
    if (!g_page_descriptors.initialized()) {
        out.print("  page descriptors not initialized\n");
        return;
    }
    struct entry {
        page_state state;
        const char* name;
        const char* color;
        size_t count;
    };
    entry entries[] = {
        {page_state::WIRED, "wired", C_RED, 0},       {page_state::ACTIVE, "active", C_YELLOW, 0},
        {page_state::INACTIVE, "inactive", C_MAG, 0}, {page_state::FREE, "free", C_BLUE, 0},
        {page_state::ZEROED, "zeroed", C_GREEN, 0},
    };
    size_t total = 0;
    for (auto& e : entries) {
        e.count = g_page_descriptors.count(e.state);
        total += e.count;
    }
    // Not memory; reported separately and excluded from the distribution.
    size_t mmio = g_page_descriptors.count(page_state::MMIO);
    if (visual(out)) {
        // Stacked distribution bar: one colored run per state, sized by share.
        constexpr int W = 52;
        out.write("  ");
        int cells_done = 0;
        size_t cum     = 0;
        for (auto& e : entries) {
            cum     = cum + e.count;
            int end = total > 0 ? static_cast<int>(cum * W / total) : 0;
            out.sgr(e.color);
            for (int i = cells_done; i < end; ++i) { out.write("█"); }
            out.reset_style();
            cells_done = end;
        }
        out.write("\n  ");
        for (auto& e : entries) {
            out.sgr(e.color);
            out.write("█");
            out.reset_style();
            out.print(" {0} {1}   ", e.name, e.count);
        }
        out.print("({0} frames)\n", total);
        out.sgr(C_DIM);
        out.print("  + {0} frames mmio/holes (not memory, excluded)\n", mmio);
        out.reset_style();
    } else {
        out.print("  wired {0}   active {1}   inactive {2}   free {3}   zeroed {4}   ({5} frames)   mmio {6}\n",
                  entries[0].count, entries[1].count, entries[2].count, entries[3].count, entries[4].count, total,
                  mmio);
    }
}

// ── heap ──────────────────────────────────────────────────

void draw_heap_map(ShellOutput& out) {
    auto s              = g_early_heap.stats();
    uint64_t heap_bytes = s.used_bytes + s.free_bytes;
    if (heap_bytes == 0) { return; }
    struct map_ctx {
        uint64_t heap_bytes;
        uint64_t pos;
        uint64_t used[STRIP_CELLS];
        uint64_t all[STRIP_CELLS];
    };
    map_ctx ctx{};
    ctx.heap_bytes = heap_bytes;
    g_early_heap.for_each_block(
        [](void* raw, size_t payload, bool is_free) {
            auto* m        = static_cast<map_ctx*>(raw);
            // Spread this block's bytes across the cells it covers.
            uint64_t start = m->pos;
            uint64_t end   = m->pos + payload;
            m->pos         = end;
            for (uint64_t at = start; at < end;) {
                size_t cell = at * STRIP_CELLS / m->heap_bytes;
                if (cell >= STRIP_CELLS) { break; }
                uint64_t cell_end = (cell + 1) * m->heap_bytes / STRIP_CELLS;
                uint64_t take     = (end < cell_end ? end : cell_end) - at;
                if (take == 0) { take = 1; }
                m->all[cell] += take;
                if (!is_free) { m->used[cell] += take; }
                at += take;
            }
        },
        &ctx);
    out.write("  ");
    for (int i = 0; i < STRIP_CELLS; ++i) {
        if (ctx.used[i] == 0) {
            out.sgr(C_DIM);
            out.write("░");
        } else if (ctx.used[i] >= ctx.all[i]) {
            out.sgr(C_CYAN);
            out.write("█");
        } else {
            out.sgr(C_CYAN);
            out.write("▒");
        }
        out.reset_style();
    }
    out.write("\n  ");
    out.sgr(C_DIM);
    out.write("block map: ");
    out.reset_style();
    out.sgr(C_CYAN);
    out.write("█");
    out.reset_style();
    out.write(" used ");
    out.sgr(C_DIM);
    out.write("░");
    out.reset_style();
    out.write(" free ▒ mixed\n");
}

void render_heap(ShellOutput& out) {
    section(out, "heap (early)");
    auto s = g_early_heap.stats();
    char a[24], b[24], c[24];
    uint64_t capacity = s.used_bytes + s.free_bytes;
    if (visual(out)) { draw_bar(out, s.used_bytes, capacity); }
    out.print("  {0} / {1} used   peak {2}\n", human_bytes(a, sizeof(a), s.used_bytes),
              human_bytes(b, sizeof(b), capacity), human_bytes(c, sizeof(c), s.peak_used));
    uint64_t frag = s.free_bytes > 0 ? (s.free_bytes - s.largest_free) * 100 / s.free_bytes : 0;
    out.print("  {0} blocks   largest free {1}   fragmentation {2}%   allocs {3}   frees {4}\n", s.blocks,
              human_bytes(a, sizeof(a), s.largest_free), frag, s.alloc_calls, s.free_calls);
    if (visual(out)) { draw_heap_map(out); }
}

// ── kernel aspace ─────────────────────────────────────────

// "rwxu" with dashes for missing bits.
const char* prot_str(vm_prot_t prot) {
    static char buf[5];
    buf[0] = (prot & vm_prot::READ) ? 'r' : '-';
    buf[1] = (prot & vm_prot::WRITE) ? 'w' : '-';
    buf[2] = (prot & vm_prot::EXECUTE) ? 'x' : '-';
    buf[3] = (prot & vm_prot::USER) ? 'u' : '-';
    buf[4] = '\0';
    return buf;
}

const char* cache_str(vm_cache_mode mode) {
    switch (mode) {
        case vm_cache_mode::DEVICE: return "device";
        case vm_cache_mode::WRITE_COMBINING: return "wc";
        case vm_cache_mode::CACHED:
        default: return "cached";
    }
}

// Box-drawing tree. prefix carries the accumulated "│  "/"   " runs; protocol
// mode gets plain two-space indentation instead.
void dump_region_tree(ShellOutput& out, const Region& region, char* prefix, size_t prefix_len, size_t prefix_cap) {
    size_t child_count = 0;
    region.for_each_child([&](const region_child&) { ++child_count; });
    size_t index = 0;
    region.for_each_child([&](const region_child& slot) {
        bool last = ++index == child_count;
        out.sgr(C_DIM);
        out.print("{0}{1}", static_cast<const char*>(prefix), visual(out) ? (last ? "└─ " : "├─ ") : "");
        out.reset_style();
        if (slot.is_binding()) {
            out.print("map [0x{0:p}, 0x{1:p}) ", slot.base, slot.base + slot.size);
            out.sgr(C_YELLOW);
            out.print("{0}", prot_str(slot.prot));
            out.reset_style();
            out.print(" {0}", cache_str(slot.cache));
            if (slot.vmo_ref.get() != nullptr) {
                const vmo& v = *slot.vmo_ref;
                out.sgr(C_CYAN);
                out.print("  vmo {0}", v.id());
                out.reset_style();
                out.print(" ({0}/{1} pg resident, {2} fills)", v.resident_pages(), v.size_pages(), v.fill_count());
            }
            out.print("\n");
        } else {
            out.print("region [0x{0:p}, 0x{1:p}) max=", slot.child->base(), slot.child->base() + slot.child->size());
            out.sgr(C_YELLOW);
            out.print("{0}", prot_str(slot.child->max_prot()));
            out.reset_style();
            out.print("\n");
            if (prefix_len + 4 < prefix_cap) {
                const char* down = visual(out) ? (last ? "   " : "│  ") : "  ";
                size_t added     = 0;
                for (const char* p = down; *p != '\0'; ++p, ++added) { prefix[prefix_len + added] = *p; }
                prefix[prefix_len + added] = '\0';
                dump_region_tree(out, *slot.child, prefix, prefix_len + added, prefix_cap);
                prefix[prefix_len] = '\0';
            }
        }
    });
}

// Deduped inventory of VMOs bound somewhere in the tree.
struct vmo_inventory {
    static constexpr size_t MAX = 64;
    const vmo* entries[MAX];
    size_t bindings[MAX];
    size_t count    = 0;
    bool overflowed = false;

    void add(const vmo* v) {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i] == v) {
                ++bindings[i];
                return;
            }
        }
        if (count == MAX) {
            overflowed = true;
            return;
        }
        entries[count]  = v;
        bindings[count] = 1;
        ++count;
    }
};

void collect_vmos(const Region& region, vmo_inventory& inv) {
    region.for_each_child([&](const region_child& slot) {
        if (slot.is_binding()) {
            if (slot.vmo_ref.get() != nullptr) { inv.add(slot.vmo_ref.get()); }
        } else {
            collect_vmos(*slot.child, inv);
        }
    });
}

void render_vm(ShellOutput& out) {
    section(out, "kernel aspace");
    vm_aspace& aspace = kernel_aspace();
    if (!aspace.has_root()) {
        out.print("  not initialized\n");
        return;
    }
    vmo_inventory inv{};
    collect_vmos(aspace.root(), inv);
    size_t resident = 0;
    size_t mappings = 0;
    for (size_t i = 0; i < inv.count; ++i) {
        resident += inv.entries[i]->resident_pages();
        mappings += inv.bindings[i];
    }
    out.print("  {0} mappings   {1} VMOs mapped   {2} resident pg   faults {3}\n", mappings, inv.count, resident,
              aspace.fault_count());
    char prefix[64] = "  ";
    out.print("  root [0x{0:p}, 0x{1:p})\n", aspace.root().base(), aspace.root().base() + aspace.root().size());
    dump_region_tree(out, aspace.root(), prefix, 2, sizeof(prefix));
}

void render_vmo(ShellOutput& out) {
    section(out, "vmo");
    vm_aspace& aspace = kernel_aspace();
    if (!aspace.has_root()) {
        out.print("  not initialized\n");
        return;
    }
    vmo_inventory inv{};
    collect_vmos(aspace.root(), inv);
    out.sgr(C_DIM);
    out.print("  {0:-6} {1:-8} {2:-10} {3:-8} {4:-6} {5}\n", "ID", "PAGES", "RESIDENT", "FILLS", "MAPS", "RES%");
    out.reset_style();
    for (size_t i = 0; i < inv.count; ++i) {
        const vmo& v = *inv.entries[i];
        uint64_t pct = v.size_pages() > 0 ? v.resident_pages() * 100 / v.size_pages() : 0;
        out.print("  {0:-6} {1:-8} {2:-10} {3:-8} {4:-6} ", v.id(), v.size_pages(), v.resident_pages(), v.fill_count(),
                  v.mapping_count());
        out.sgr(util_color(v.resident_pages(), v.size_pages()));
        out.print("{0}%\n", pct);
        out.reset_style();
    }
    if (inv.overflowed) { out.print("  (table full; more VMOs not shown)\n"); }
    uint32_t live = kernel::obj::g_type_registry.live_count(vmo::TYPE_ID);
    out.print("  {0} mapped of {1} live", inv.count, live);
    if (live >= inv.count) { out.print("   ({0} unmapped)", live - inv.count); }
    out.print("\n");
}

void render_all(ShellOutput& out) {
    render_phys(out);
    render_pages(out);
    render_heap(out);
    render_vm(out);
    render_vmo(out);
}

// ── mem top ───────────────────────────────────────────────

constexpr size_t SPARK_LEN = 40;

void draw_sparkline(ShellOutput& out, const uint64_t* ring, size_t len, size_t head) {
    static const char* LEVELS[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    uint64_t lo = UINT64_MAX, hi = 0;
    for (size_t i = 0; i < len; ++i) {
        if (ring[i] < lo) { lo = ring[i]; }
        if (ring[i] > hi) { hi = ring[i]; }
    }
    if (len == 0) { return; }
    uint64_t span = hi > lo ? hi - lo : 1;
    out.sgr(C_CYAN);
    // Oldest first: head points at the next overwrite slot.
    for (size_t i = 0; i < len; ++i) {
        uint64_t v = ring[(head + SPARK_LEN - len + i) % SPARK_LEN];
        out.write(LEVELS[(v - lo) * 7 / span]);
    }
    out.reset_style();
}

void cmd_top(ShellOutput& out) {
    if (out.protocol_mode()) {
        out.print("mem top: interactive only; disabled in protocol mode\n");
        return;
    }
    uint64_t hz = kernel::arch::timestamp_hz();
    uint64_t free_ring[SPARK_LEN];
    uint64_t fault_ring[SPARK_LEN];
    size_t ring_len = 0, ring_head = 0;
    uint64_t prev_ts = 0, prev_faults = 0, prev_allocs = 0, prev_zeroer = 0;
    bool first = true;
    for (;;) {
        while (uart.received_data() != 0) {
            char c = uart.read();
            if (c == 'x' || c == 'q') {
                out.write("\x1b[0m\n");
                return;
            }
        }
        auto s          = kernel::mm::g_page_frame_allocator.stats();
        uint64_t faults = kernel_aspace().has_root() ? kernel_aspace().fault_count() : 0;
        uint64_t now    = kernel::arch::timestamp();

        out.write("\x1b[H\x1b[2J");
        out.sgr(C_TITLE);
        out.write("mem top");
        out.sgr(C_DIM);
        out.write("  --  press x to exit\n");
        out.reset_style();

        uint64_t fault_rate = 0;
        if (!first && now > prev_ts && hz > 0) {
            uint64_t dt_ms = (now - prev_ts) * 1000 / hz;
            if (dt_ms > 0) {
                fault_rate           = (faults - prev_faults) * 1000 / dt_ms;
                uint64_t alloc_rate  = (s.alloc_count - prev_allocs) * 1000 / dt_ms;
                uint64_t zeroer_rate = (s.zeroer_pages - prev_zeroer) * 1000 / dt_ms;
                out.print("faults/s {0}   allocs/s {1}   zeroer pg/s {2}\n", fault_rate, alloc_rate, zeroer_rate);
            }
        } else {
            out.write("\n");
        }

        free_ring[ring_head]  = s.free_pages;
        fault_ring[ring_head] = fault_rate;
        ring_head             = (ring_head + 1) % SPARK_LEN;
        if (ring_len < SPARK_LEN) { ring_len++; }

        out.write("free  ");
        draw_sparkline(out, free_ring, ring_len, ring_head);
        char buf[24];
        out.print("  {0}\n", human_pages(buf, sizeof(buf), s.free_pages));
        out.write("flt/s ");
        draw_sparkline(out, fault_ring, ring_len, ring_head);
        out.print("  {0}\n", fault_rate);

        render_all(out);

        prev_ts     = now;
        prev_faults = faults;
        prev_allocs = s.alloc_count;
        prev_zeroer = s.zeroer_pages;
        first       = false;
        kernel::sched::sleep_ticks(500);
    }
}

void mem_handler(int argc, const ktl::string_view argv[], ShellOutput& output) {
    if (argc < 2) {
        render_all(output);
        return;
    }
    if (argv[1] == "phys") {
        render_phys(output);
    } else if (argv[1] == "pages") {
        render_pages(output);
    } else if (argv[1] == "heap") {
        render_heap(output);
    } else if (argv[1] == "vm") {
        render_vm(output);
    } else if (argv[1] == "vmo") {
        render_vmo(output);
    } else if (argv[1] == "top") {
        cmd_top(output);
    } else {
        output.print("usage: mem [phys|pages|heap|vm|vmo|top]\n");
    }
}

}  // namespace

KSHELL_COMMAND(mem, "mem", "Memory debug view: physical, pages, heap, aspace, VMOs", mem_handler);

#endif  // CONFIG_KERNEL_SHELL
