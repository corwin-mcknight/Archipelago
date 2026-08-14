#pragma once

#include <kernel/sched/scheduler.h>

namespace kernel::testing {

// Spawn a kernel thread running `fn`, a callable held by reference on the caller's frame -- so
// tests can pass capturing lambdas without a context struct + void* trampoline. The caller's
// frame must outlive the thread: wait for completion before `fn` goes out of scope.
template <typename F> ktl::result<ktl::ref<sched::Thread>> spawn_fn(const char* name, F& fn) {
    return sched::spawn(name, [](void* arg) { (*static_cast<F*>(arg))(); }, &fn);
}

}  // namespace kernel::testing
