# The per-architecture user-space ABI contract, shared by the crt build (Makefile) and every user
# program (user.mk includes it from the sysroot, where it is installed alongside). libcrt.a and the
# programs that link it must agree on these flags exactly -- an lp64/lp64d or SSE mismatch breaks
# the link -- so the definition lives once, here.
ifeq ($(ARCH),x86_64)
TRIPLE ?= x86_64-unknown-none
# Standard x86_64 ABI, vector instructions included: the kernel enables SSE for user mode and
# carries FP/SIMD state per thread across context switches.
ARCH_FLAGS   := -m64 --target=$(TRIPLE)
LD_EMULATION := elf_x86_64
else ifeq ($(ARCH),riscv64)
TRIPLE ?= riscv64-unknown-none
# Standard lp64d ABI, F/D included: the kernel enables sstatus.FS and carries the f-register file
# plus fcsr per thread across context switches.
ARCH_FLAGS   := --target=$(TRIPLE) -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -mcmodel=medany
LD_EMULATION := elf64lriscv
else
$(error unsupported ARCH '$(ARCH)')
endif
