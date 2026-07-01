// ThreadSanitizer stress harness for kernel::log_ring (host TSan lane, step 6).
//
// log_ring is a lock-free multi-producer/single-consumer bounded ring. Its correctness rests on the
// producer's publish() (release store of the slot state) synchronizing-with the flusher's drain()
// (acquire load of that state): the release makes the producer's *plain* payload write visible to the
// consumer without a data race. Downgrade either side to relaxed and the payload read in drain()
// becomes a race -- invisible on strong-ordered x86 but a real bug on weak hardware. TSan catches it
// here regardless of the host's own ordering. This harness drives the real reserve/publish/drain paths
// from real pthreads so TSan can prove those synchronizes-with edges hold.

#include <kernel/log_ring.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

namespace {

// Payload carries a value and its complement so a torn (half-published) read is also a logic failure,
// not only a TSan report -- belt and suspenders.
struct payload {
    uint64_t v{0};
    uint64_t inv{0};
};

constexpr int kProducers      = 4;
constexpr uint64_t kPerThread = 20000;
constexpr uint64_t kExpected  = kProducers * kPerThread;

kernel::log_ring<payload, 256> g_ring;
bool g_torn = false;

void* producer(void* arg) {
    uint64_t base = (uint64_t)(uintptr_t)arg * kPerThread;
    for (uint64_t i = 0; i < kPerThread; i++) {
        uint64_t seq;
        payload* p;
        // Ring is smaller than the message count, so full is expected -- spin until the single
        // consumer drains a slot free. This backpressure is what puts producers and consumer in
        // contention on the same slots, which is what TSan needs to see.
        while ((p = g_ring.reserve(seq)) == nullptr) { /* full: let the consumer catch up */
        }
        uint64_t val = base + i;
        p->v         = val;
        p->inv       = ~val;
        g_ring.publish(seq);
    }
    return nullptr;
}

}  // namespace

int main() {
    pthread_t prod[kProducers];
    for (int t = 0; t < kProducers; t++) { pthread_create(&prod[t], nullptr, producer, (void*)(uintptr_t)t); }

    uint64_t seen = 0;
    while (seen < kExpected) {
        g_ring.drain([&](const payload& p) {
            if (p.inv != ~p.v) { g_torn = true; }
            seen++;
        });
    }

    for (auto& th : prod) { pthread_join(th, nullptr); }

    if (g_torn) {
        fprintf(stderr, "log-ring: torn/unsynchronized payload read\n");
        return 1;
    }
    if (seen != kExpected) {
        fprintf(stderr, "log-ring: lost messages -- drained %llu, want %llu\n", (unsigned long long)seen,
                (unsigned long long)kExpected);
        return 1;
    }
    printf("tsan: log_ring MPSC drain passed (%llu messages)\n", (unsigned long long)seen);
    return 0;
}
