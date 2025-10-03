#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

/// Represents the state of a physical page frame (stored in 4 bits).
enum class vm_page_state : uint8_t {
    FREE     = 0b0000,
    ACTIVE   = 0b0001,
    INACTIVE = 0b0010,
    WIRED    = 0b0011,
    ZEROED   = 0b0100,
};

// TODO: Add more page sizes as needed. Kernel assumes 4K pages for now.
enum class vm_page_size : uint8_t {
    SIZE_4K = 0,
};

typedef uintptr_t vm_paddr_t;
typedef uintptr_t vm_page_id_t;

/// Packed representation of a physical page. The lower 12 bits store metadata,
/// and the remaining bits store the page frame number.
struct vm_page {
    vm_paddr_t m_data{0};

    static constexpr uint8_t metadata_bits       = 12;
    static constexpr uint8_t state_bits          = 4;
    static constexpr uint8_t flag_bits           = metadata_bits - state_bits;
    static constexpr uint8_t frame_bits          = 64 - metadata_bits;  // TWEAK: Assumes 64-bit pointers.

    static constexpr vm_paddr_t state_mask       = (1ull << state_bits) - 1ull;
    static constexpr vm_paddr_t flag_value_mask  = (1ull << flag_bits) - 1ull;
    static constexpr vm_paddr_t flag_mask        = flag_value_mask << state_bits;
    static constexpr vm_paddr_t frame_value_mask = (1ull << frame_bits) - 1ull;
    static constexpr vm_paddr_t frame_mask       = frame_value_mask << metadata_bits;
    static constexpr vm_page_id_t max_frame_id   = static_cast<vm_page_id_t>(frame_value_mask);

    explicit vm_page(vm_paddr_t _address, vm_page_state _state = vm_page_state::FREE, uint8_t _flags = 0) {
        set_physical_address(_address);
        set_state(_state);
        set_flags(_flags);
    }

    [[nodiscard]] vm_page_state state() const { return static_cast<vm_page_state>(m_data & state_mask); }

    void set_state(vm_page_state state) { m_data = (m_data & ~state_mask) | static_cast<uint64_t>(state); }

    [[nodiscard]] uint8_t flags() const { return static_cast<uint8_t>((m_data & flag_mask) >> state_bits); }

    void set_flags(uint8_t flags) {
        const uint64_t masked = static_cast<uint64_t>(flags) & flag_value_mask;
        m_data                = (m_data & ~flag_mask) | (masked << state_bits);
    }

    [[nodiscard]] vm_page_id_t frame_id() const {
        return static_cast<vm_page_id_t>((m_data & frame_mask) >> metadata_bits);
    }

    void set_frame_id(vm_page_id_t frame_id) {
        const uint64_t masked = static_cast<uint64_t>(frame_id) & frame_value_mask;
        m_data                = (m_data & ~frame_mask) | (masked << metadata_bits);
    }

    [[nodiscard]] vm_paddr_t physical_address() const { return static_cast<vm_paddr_t>(frame_id()) << metadata_bits; }

    void set_physical_address(vm_paddr_t physical_address) {
        set_frame_id(static_cast<vm_page_id_t>(physical_address >> metadata_bits));
    }
};

static_assert(sizeof(vm_page) == sizeof(vm_paddr_t), "vm_page must pack into a pointer size");

struct vm_page_region {};
}  // namespace kernel::mm
