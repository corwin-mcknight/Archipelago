// Host-tier test runner: the fork-per-test backend.
//
// The parent walks the .ktests registry (built once at load) and forks a child per test. Each child
// runs one test in a pristine address space -- no teardown, clean crash attribution, cheap death
// tests -- and emits @@HARNESS events to stdout. The parent reaps each child and, when a child dies
// by signal (a real fault, or a sanitizer abort), synthesizes the missing test_end so every test has
// a result. This is the host transport for the shared @@HARNESS protocol; the Python aggregator
// consumes the same events the QEMU tier emits over serial.

#include <kernel/panic.h>
#include <kernel/testing/registry.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" kernel::testing::ktest __start__ktests[];
extern "C" kernel::testing::ktest __stop__ktests[];

namespace {

jmp_buf g_test_jmp;
bool g_test_failed = false;

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void emit_raw(const char* json) {
    fputs("@@HARNESS ", stdout);
    fputs(json, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// Escape a string for use inside a JSON double-quoted value.
void json_escape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    auto put = [&](char c) {
        if (o + 1 < cap) { out[o++] = c; }
    };
    for (size_t i = 0; in && in[i]; ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        switch (c) {
            case '"':
                put('\\');
                put('"');
                break;
            case '\\':
                put('\\');
                put('\\');
                break;
            case '\n':
                put('\\');
                put('n');
                break;
            case '\r':
                put('\\');
                put('r');
                break;
            case '\t':
                put('\\');
                put('t');
                break;
            default:
                if (c < 0x20) {
                    char u[8];
                    snprintf(u, sizeof u, "\\u%04x", c);
                    for (int k = 0; u[k]; ++k) { put(u[k]); }
                } else {
                    put(static_cast<char>(c));
                }
        }
    }
    out[o < cap ? o : cap - 1] = '\0';
}

void emit_error(const char* message) {
    char esc[1024];
    json_escape(message, esc, sizeof esc);
    char line[1100];
    snprintf(line, sizeof line, "{\"event\":\"error\",\"message\":\"%s\"}", esc);
    emit_raw(line);
}

void emit_test_start(const char* name) {
    char line[256];
    snprintf(line, sizeof line, "{\"event\":\"test_start\",\"name\":\"%s\",\"timestamp\":%llu}", name,
             static_cast<unsigned long long>(now_ns()));
    emit_raw(line);
}

void emit_test_end(const char* name, const char* status, const char* reason, uint64_t duration_ns) {
    char line[1200];
    if (reason) {
        char esc[1024];
        json_escape(reason, esc, sizeof esc);
        snprintf(line, sizeof line,
                 "{\"event\":\"test_end\",\"name\":\"%s\",\"status\":\"%s\",\"reason\":\"%s\",\"duration_ns\":%llu}",
                 name, status, esc, static_cast<unsigned long long>(duration_ns));
    } else {
        snprintf(line, sizeof line, "{\"event\":\"test_end\",\"name\":\"%s\",\"status\":\"%s\",\"duration_ns\":%llu}",
                 name, status, static_cast<unsigned long long>(duration_ns));
    }
    emit_raw(line);
}

void emit_test_meta_rss(const char* name, long peak_rss_kb) {
    char line[256];
    snprintf(line, sizeof line, "{\"event\":\"test_meta\",\"name\":\"%s\",\"peak_rss_kb\":%ld}", name, peak_rss_kb);
    emit_raw(line);
}

bool expects_crash(const kernel::testing::ktest& t) {
    return (t.flags & kernel::testing::KTEST_FLAG_EXPECTS_CRASH) != 0;
}

// Runs one test in the forked child and returns the child exit code (0 pass, 1 fail).
int run_test_child(const kernel::testing::ktest& t) {
    g_test_failed  = false;
    uint64_t start = now_ns();
    emit_test_start(t.name);
    if (setjmp(g_test_jmp) == 0) {
        if (t.init_fn) { t.init_fn(); }
        t.fn();
    }
    // A longjmp lands here (fatal REQUIRE, abort, or panic) with g_test_failed already set.
    uint64_t dur = now_ns() - start;

    if (expects_crash(t)) {
        // On host a "crash" surfaces as a caught longjmp; completing cleanly means the crash never came.
        if (g_test_failed) {
            emit_test_end(t.name, "pass", nullptr, dur);
            return 0;
        }
        emit_test_end(t.name, "fail", "expected crash but test completed", dur);
        return 1;
    }
    if (g_test_failed) {
        emit_test_end(t.name, "fail", "assertion failed (see error events)", dur);
        return 1;
    }
    emit_test_end(t.name, "pass", nullptr, dur);
    return 0;
}

bool name_selected(const kernel::testing::ktest& t, int argc, char** argv) {
    if (argc <= 1) { return true; }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], t.name) == 0) { return true; }
    }
    return false;
}

}  // namespace

// ---- Backend symbols required by the testing layer and KTL ----

namespace kernel::testing {

void report_assertion(bool passed, bool fatal, const char* file, int line, const char* expr, const char* lhs,
                      const char* op, const char* rhs) {
    if (passed) { return; }
    g_test_failed = true;
    char msg[768];
    if (op && op[0]) {
        snprintf(msg, sizeof msg, "%s  (%s %s %s)  at %s:%d", expr, lhs, op, rhs, file, line);
    } else {
        snprintf(msg, sizeof msg, "%s  (= %s)  at %s:%d", expr, lhs, file, line);
    }
    emit_error(msg);
    if (fatal) { abort(1); }
}

void abort(unsigned char exit_code) {
    g_test_failed = true;
    longjmp(g_test_jmp, exit_code ? static_cast<int>(exit_code) : 1);
}

}  // namespace kernel::testing

// KTL reaches for the global panic()/hcf() (e.g. string_view bounds checks). On the host these record
// a failure and unwind to the runner rather than killing the process.
void panic(const char* s) {
    g_test_failed = true;
    char msg[512];
    snprintf(msg, sizeof msg, "panic: %s", s ? s : "(null)");
    emit_error(msg);
    longjmp(g_test_jmp, 1);
}

void hcf() { longjmp(g_test_jmp, 1); }

// Kernel synchronization (Spinlock, used by e.g. HandleTable) reaches for the x86 interrupt-state ops.
// The host runner is single-threaded per forked test, so the lock is a no-op -- these stubs just
// satisfy the linker. Real lock/interrupt behaviour is exercised on the freestanding (QEMU) tier.
namespace kernel::x86 {
uint64_t save_and_disable_interrupts() { return 0; }
void restore_interrupts(uint64_t) {}
}  // namespace kernel::x86

// Defined by the LLVM coverage runtime only in coverage builds (-fprofile-instr-generate). The child
// _exit()s, which skips the runtime's atexit writer, so we flush this child's counters explicitly.
// Weak: in a normal (non-coverage) build the symbol is absent and the call is skipped.
extern "C" __attribute__((weak)) int __llvm_profile_write_file(void);

int main(int argc, char** argv) {
    int total = 0, passed = 0, failed = 0;
    for (auto* t = __start__ktests; t != __stop__ktests; ++t) {
        if (!name_selected(*t, argc, argv)) { continue; }
        ++total;
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            int rc = run_test_child(*t);
            fflush(stdout);
            if (__llvm_profile_write_file) { __llvm_profile_write_file(); }
            _exit(rc);
        }
        int status = 0;
        struct rusage ru;
        wait4(pid, &status, 0, &ru);
        bool ok;
        if (WIFEXITED(status)) {
            ok = (WEXITSTATUS(status) == 0);
        } else if (WIFSIGNALED(status)) {
            // Child faulted or a sanitizer aborted: it never emitted test_end, so synthesize one.
            int sig = WTERMSIG(status);
            if (expects_crash(*t)) {
                emit_test_end(t->name, "pass", nullptr, 0);
                ok = true;
            } else {
                char reason[128];
                snprintf(reason, sizeof reason, "child terminated by signal %d", sig);
                emit_test_end(t->name, "fail", reason, 0);
                ok = false;
            }
        } else {
            emit_test_end(t->name, "fail", "child did not exit normally", 0);
            ok = false;
        }
        // ru_maxrss (Linux: KB) is this child's peak RSS. Each test is a fresh fork, so it includes a
        // constant inherited baseline -- consistent across tests, so cross-test comparison is meaningful.
        emit_test_meta_rss(t->name, ru.ru_maxrss);
        if (ok) {
            ++passed;
        } else {
            ++failed;
        }
    }
    char summary[160];
    snprintf(summary, sizeof summary, "{\"event\":\"run_end\",\"total\":%d,\"passed\":%d,\"failed\":%d}", total, passed,
             failed);
    emit_raw(summary);
    printf("%d tests: %d passed, %d failed\n", total, passed, failed);
    return failed ? 1 : 0;
}
