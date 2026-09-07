#include <abi/syscall.h>
#include <elf/loader.h>
#include <sys.h>

namespace elf {
namespace {
constexpr uint64_t PAGE          = ABI_VM_PAGE_SIZE;
constexpr uint64_t STACK_BASE    = 0x800000;
constexpr uint64_t STACK_BYTES   = 4 * PAGE;
constexpr uint64_t DESCRIPTOR_AT = 128;

uint64_t prepare_mapping(uint64_t factory, uint64_t address, uint64_t size, uint64_t prot, const uint8_t* bytes,
                         uint64_t file_size, char* ipc) {
    uint64_t vmo = sys_vmo_create(size);
    if (sys_is_error(vmo)) { return vmo; }
    if (file_size != 0) {
        uint64_t mapping = sys_vmo_map(vmo, 0, 0, size, ABI_VM_PROT_READ | ABI_VM_PROT_WRITE);
        if (sys_is_error(mapping)) {
            (void)sys_handle_close(vmo);
            return mapping;
        }
        auto* destination = reinterpret_cast<uint8_t*>(mapping);
        for (uint64_t i = 0; i < file_size; ++i) { destination[i] = bytes[i]; }
        (void)sys_vmo_unmap(mapping);
    }
    abi_task_build_mapping descriptor{address, size, 0, prot};
    __builtin_memcpy(ipc + DESCRIPTOR_AT, &descriptor, sizeof(descriptor));
    uint64_t result = sys_task_build_map(factory, vmo, DESCRIPTOR_AT);
    (void)sys_handle_close(vmo);
    return result;
}
}  // namespace

uint64_t spawn(uint64_t factory, uint64_t image_handle, uint64_t image_size, const char* name, size_t name_size,
               uint64_t output_offset, char* ipc) {
    if (ipc == nullptr) { ipc = sys_ipc_base(); }
    if (image_size == 0 || image_size > MAX_IMAGE_BYTES || name == nullptr || name_size == 0 || name_size > 63) {
        return static_cast<uint64_t>(ABI_ERR_INVALID_OPERATION);
    }
    uint64_t rounded = (image_size + PAGE - 1) & ~(PAGE - 1);
    uint64_t mapping = sys_vmo_map(image_handle, 0, 0, rounded, ABI_VM_PROT_READ);
    if (sys_is_error(mapping)) { return mapping; }
    const auto* bytes = reinterpret_cast<const uint8_t*>(mapping);
    auto parsed       = parse_image(bytes, image_size);
    if (parsed.is_err()) {
        (void)sys_vmo_unmap(mapping);
        return static_cast<uint64_t>(ABI_ERR_INVALID_OPERATION);
    }
    __builtin_memcpy(ipc, name, name_size);
    uint64_t result = sys_task_build_create(factory, 0, name_size);
    if (sys_is_error(result)) {
        (void)sys_vmo_unmap(mapping);
        return result;
    }
    const auto& img = parsed.value;
    for (size_t i = 0; i < img.count; ++i) {
        const auto& segment = img.segments[i];
        uint64_t prot       = ABI_VM_PROT_READ;
        if ((segment.flags & PF_W) != 0) { prot |= ABI_VM_PROT_WRITE; }
        if ((segment.flags & PF_X) != 0) { prot |= ABI_VM_PROT_EXEC; }
        result = prepare_mapping(factory, segment.vaddr, (segment.memsz + PAGE - 1) & ~(PAGE - 1), prot,
                                 bytes + segment.file_offset, segment.filesz, ipc);
        if (sys_is_error(result)) { break; }
    }
    (void)sys_vmo_unmap(mapping);
    if (!sys_is_error(result)) {
        result =
            prepare_mapping(factory, STACK_BASE, STACK_BYTES, ABI_VM_PROT_READ | ABI_VM_PROT_WRITE, nullptr, 0, ipc);
    }
    if (!sys_is_error(result)) {
        result = sys_task_build_start(factory, img.entry, STACK_BASE + STACK_BYTES, output_offset);
    }
    if (sys_is_error(result)) { (void)sys_task_build_abort(factory); }
    return result;
}

}  // namespace elf
