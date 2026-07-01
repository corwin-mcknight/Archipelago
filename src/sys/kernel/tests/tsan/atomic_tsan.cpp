// ThreadSanitizer stress harness for ktl::atomic (host TSan lane, step 6).
//
// The fork-per-test host runner is single-threaded, so it structurally cannot exercise a data race;
// this is a separate binary that drives ktl::atomic from real pthreads under TSan. TSan models the
// C++/builtin memory model abstractly, so it flags missing synchronization regardless of the host's
// own memory ordering -- which is the whole point: a `relaxed` used where `release`/`acquire` is
// required is invisible on strong-ordered hardware but a real bug, and TSan catches it here.
//
// Two scenarios:
//   counter      -- N threads fetch_add a shared atomic; the sum proves atomicity (no lost updates).
//   message-pass -- a producer writes plain data then store(release)s a flag; a consumer spins on
//                   load(acquire) then reads the data. TSan-clean proves the release/acquire pair
//                   actually synchronizes the non-atomic data; downgrade either to relaxed and TSan
//                   reports a data race on `g_payload`. This scenario IS the lane's reason to exist.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include <ktl/atomic>

using ktl::atomic;
using ktl::memory_order;

namespace {

constexpr int kThreads        = 8;
constexpr uint64_t kPerThread = 100000;

atomic<uint64_t> g_counter{0};

void* counter_worker(void*) {
    for (uint64_t i = 0; i < kPerThread; i++) { g_counter.fetch_add(1, memory_order::relaxed); }
    return nullptr;
}

bool run_counter() {
    g_counter.store(0);
    pthread_t t[kThreads];
    for (auto& th : t) { pthread_create(&th, nullptr, counter_worker, nullptr); }
    for (auto& th : t) { pthread_join(th, nullptr); }
    uint64_t got  = g_counter.load();
    uint64_t want = kThreads * kPerThread;
    if (got != want) {
        fprintf(stderr, "counter: lost updates -- got %llu, want %llu\n", (unsigned long long)got,
                (unsigned long long)want);
        return false;
    }
    return true;
}

// Plain (non-atomic) payload, published across threads only via the release/acquire flag below.
uint64_t g_payload = 0;
atomic<uint32_t> g_ready{0};

void* producer(void*) {
    g_payload = 0xdeadbeefcafef00d;  // plain store, ordered before the release
    g_ready.store(1, memory_order::release);
    return nullptr;
}

bool run_message_pass() {
    bool ok = true;
    for (int iter = 0; iter < 1000; iter++) {
        g_payload = 0;
        g_ready.store(0);
        pthread_t prod;
        pthread_create(&prod, nullptr, producer, nullptr);
        while (g_ready.load(memory_order::acquire) == 0) { /* spin until published */
        }
        uint64_t seen = g_payload;  // safe to read non-atomically: the acquire synchronized-with the release
        pthread_join(prod, nullptr);
        if (seen != 0xdeadbeefcafef00d) {
            fprintf(stderr, "message-pass: torn/unsynchronized read 0x%llx\n", (unsigned long long)seen);
            ok = false;
            break;
        }
    }
    return ok;
}

}  // namespace

int main() {
    bool ok = run_counter();
    ok      = run_message_pass() && ok;
    if (ok) { printf("tsan: 2 scenarios passed\n"); }
    return ok ? 0 : 1;
}
