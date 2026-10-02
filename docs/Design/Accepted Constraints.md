# Accepted Constraints

These describe current policy, not unfinished implementation.

- Bootstrap channels are parent-to-task. Only the coordinator's parent end is held by the kernel; PEER_CLOSED observes parent death. A child can retain up to the bounded mailbox queue until teardown. A future kernel control plane needs its own explicit interface.
- Killed threads do not park again on signal waits after the kill scan. A killed thread contending on a kernel mutex may busy-wait with preemption until it reaches the syscall exit boundary.
- Anonymous memory is not swapped. Fallible allocation returns an error/null; boot allocation and ordinary `operator new` retain panic-on-OOM contracts.
- User VMO creation has no policy size cap until quotas exist; allocation may fail during metadata construction or later at commit/touch. Direct PMM clients also include kernel stacks, heap/arena backing, channel pages, and page tables -- VMM is not the sole PMM consumer and does not currently provide a general reclaim/retry policy.
