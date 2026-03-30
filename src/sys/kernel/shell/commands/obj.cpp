#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/config.h>
#include <kernel/obj/type_registry.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>

namespace {

void obj_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: obj types\n");
        return;
    }
    ktl::string_view sub(argv[1]);
    if (sub == "types") {
        output.print("Registered types: {0}\n", kernel::obj::g_type_registry.count());
        for (size_t i = 0; i < CONFIG_MAX_OBJECT_TYPES; ++i) {
            auto desc = kernel::obj::g_type_registry.lookup(static_cast<kernel::obj::TypeId>(i));
            if (desc.has_value()) {
                auto* d = desc.value();
                output.print("  [{0}] {1} (live: {2})\n", d->id, d->name,
                             kernel::obj::g_type_registry.live_count(d->id));
            }
        }
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(obj, "obj", "Object type registry inspection", obj_handler);

#endif  // CONFIG_KERNEL_SHELL
