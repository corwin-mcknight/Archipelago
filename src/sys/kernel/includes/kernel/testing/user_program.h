#pragma once

#include <kernel/mm/physmap.h>
#include <kernel/sched/task_construction.h>
#include <kernel/sched/user_task.h>
#include <kernel/syscall.h>

namespace kernel::testing {

// Test-only native instruction fixture. Kernel lifecycle tests construct mappings directly;
// executable-format parsing is exercised in userspace and in the host parser tests.
class UserProgram {
   public:
    const uint8_t* data() const { return m_bytes; }
    size_t size() const { return m_size; }

    void syscall(uint32_t number, uint32_t arg = 0) {
#if defined(ARCH_X86_64)
        byte(0xb8);
        word(number);
        byte(0xbf);
        word(arg);
        byte(0x0f);
        byte(0x05);
#elif defined(ARCH_RISCV64)
        word((number << 20) | 0x893);
        word((arg << 20) | 0x513);
        word(0x73);
#endif
    }

    void block_on_port() {
        syscall(ABI_SYS_PORT_CREATE);
#if defined(ARCH_X86_64)
        byte(0x48);
        byte(0x89);
        byte(0xc7);  // mov rdi, rax
        byte(0x31);
        byte(0xf6);
        byte(0x31);
        byte(0xd2);  // zero offset and timeout
        byte(0xb8);
        word(ABI_SYS_PORT_WAIT);
        byte(0x0f);
        byte(0x05);
#elif defined(ARCH_RISCV64)
        word(0x593);
        word(0x613);
        word((ABI_SYS_PORT_WAIT << 20) | 0x893);
        word(0x73);
#endif
    }

    void fault() {
#if defined(ARCH_X86_64)
        byte(0x31);
        byte(0xc0);
        byte(0xc6);
        byte(0x00);
        byte(0x00);  // store through null
#elif defined(ARCH_RISCV64)
        word(0x00000293);
        word(0x00028023);
#endif
    }

    ktl::result<ktl::ref<sched::Task>> start(const char* name) {
        sched::TaskConstruction pending;
        size_t length = 0;
        while (name[length]) { ++length; }
        auto initialized = pending.init(name, length);
        if (initialized.is_err()) { return ktl::err(initialized.unwrap_err()); }
        auto memory = mm::create_anonymous_vmo(1);
        if (!memory) { return ktl::err(ktl::errc::oom); }
        auto committed = memory->commit(0, 1);
        if (committed.is_err()) { return ktl::err(committed.unwrap_err()); }
        auto frame = memory->resident_frame(0);
        if (!frame) { return ktl::err(ktl::errc::oom); }
        mm::copy_to_frame(*frame, 0, m_bytes, m_size);
        auto code = pending.map(*memory, 0, 0x400000, 4096, mm::vm_prot::READ | mm::vm_prot::EXECUTE);
        if (code.is_err()) { return ktl::err(code.unwrap_err()); }
        mm::zero_frame(*frame);
        auto stack = pending.map(*memory, 0, 0x800000, 4096, mm::vm_prot::READ | mm::vm_prot::WRITE);
        if (stack.is_err()) { return ktl::err(stack.unwrap_err()); }
        return sched::start_prepared_user_task(name, pending.release(), 0x400000, 0x801000);
    }

   private:
    void byte(uint8_t value) { m_bytes[m_size++] = value; }
    void word(uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) { byte(static_cast<uint8_t>(value >> (8 * i))); }
    }
    uint8_t m_bytes[256] = {};
    size_t m_size        = 0;
};

}  // namespace kernel::testing
