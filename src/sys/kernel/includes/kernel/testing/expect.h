#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/registry.h>
#include <stddef.h>

#include <ktl/fmt_builtin>
#include <ktl/type_traits>

// Expression-capturing assertions. EXPECT(expr) / REQUIRE(expr) decompose the top-level binary
// operator of expr, capture both operand values, and route a structured pass/fail record to the
// active backend (kernel shell or host runner) via kernel::testing::report_assertion. EXPECT is
// non-fatal; REQUIRE aborts the current test on failure. The decomposition works by binding the
// left operand with operator<= -- which sits between comparison and logical precedence -- so
// `capture_start{} <= a == b` parses as `(capture_start{} <= a) == b`. && and || bind below the
// capture and are deliberately rejected at compile time.

namespace kernel::testing::detail {

inline constexpr size_t OPERAND_CAP = 48;

template <typename T> using bare_t  = ktl::remove_cv_t<ktl::remove_reference_t<T>>;

// Detect whether ktl::format::kfmt_printer<V> can print a value of type V. Note the index argument:
// ktl::declval<T>() always yields an rvalue (it strips the reference), so declval<size_t&>() would be
// a size_t&& that cannot bind to the printer's size_t& parameter and the probe would always fail.
// Dereferencing a size_t* rvalue yields a genuine size_t lvalue, which binds correctly.
template <typename V, typename = void> struct has_printer : ktl::false_type {};
template <typename V>
struct has_printer<V, ktl::void_t<decltype(ktl::format::kfmt_printer<V>::print(
                          ktl::declval<ktl::format::format_args>(), ktl::declval<char*>(), ktl::declval<size_t>(),
                          *ktl::declval<size_t*>(), ktl::declval<V>()))>> : ktl::true_type {};

inline void copy_str(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) { dst[i] = src[i]; }
    dst[i] = '\0';
}

// Format a single operand value into buf using the shared kfmt_printer machinery, falling back to a
// typed placeholder when the type has no printer. bool is spelled out for readability.
template <typename T> void format_operand(char* buf, size_t cap, const T& value) {
    if (cap == 0) { return; }
    using V = bare_t<T>;
    if constexpr (ktl::is_same_v<V, bool>) {
        copy_str(buf, cap, value ? "true" : "false");
    } else if constexpr (has_printer<V>::value) {
        ktl::format::format_args fa{};
        fa.specifier = 'd';
        size_t index = 0;
        ktl::format::kfmt_printer<V>::print(fa, buf, cap, index, value);
        buf[index < cap ? index : cap - 1] = '\0';
    } else {
        copy_str(buf, cap, "<?>");
    }
}

struct decomposed {
    bool passed;
    char lhs[OPERAND_CAP];
    char op[4];
    char rhs[OPERAND_CAP];
};

template <typename L> struct lhs_capture {
    const L& lhs;

    template <typename R> decomposed make(const char* op, bool passed, const R& rhs) const {
        decomposed d;
        d.passed = passed;
        format_operand(d.lhs, OPERAND_CAP, lhs);
        copy_str(d.op, sizeof(d.op), op);
        format_operand(d.rhs, OPERAND_CAP, rhs);
        return d;
    }

    template <typename R> decomposed operator==(const R& rhs) const { return make("==", lhs == rhs, rhs); }
    template <typename R> decomposed operator!=(const R& rhs) const { return make("!=", lhs != rhs, rhs); }
    template <typename R> decomposed operator<(const R& rhs) const { return make("<", lhs < rhs, rhs); }
    template <typename R> decomposed operator<=(const R& rhs) const { return make("<=", lhs <= rhs, rhs); }
    template <typename R> decomposed operator>(const R& rhs) const { return make(">", lhs > rhs, rhs); }
    template <typename R> decomposed operator>=(const R& rhs) const { return make(">=", lhs >= rhs, rhs); }

    // && and || cannot be captured (they bind below operator<=); reject them so the author splits
    // the expression into separate assertions or parenthesizes the operands.
    template <typename R> void operator&&(const R&) const = delete;
    template <typename R> void operator||(const R&) const = delete;

    decomposed unary() const {
        decomposed d;
        d.passed = static_cast<bool>(lhs);
        format_operand(d.lhs, OPERAND_CAP, lhs);
        d.op[0]  = '\0';
        d.rhs[0] = '\0';
        return d;
    }
};

struct capture_start {
    template <typename L> lhs_capture<L> operator<=(const L& lhs) const { return lhs_capture<L>{lhs}; }
};

inline void run_assert(const decomposed& d, bool fatal, const char* file, int line, const char* expr) {
    report_assertion(d.passed, fatal, file, line, expr, d.lhs, d.op, d.rhs);
}

// Unary case: no comparison operator was applied, so the captured value's truthiness is the result.
template <typename L>
void run_assert(const lhs_capture<L>& c, bool fatal, const char* file, int line, const char* expr) {
    run_assert(c.unary(), fatal, file, line, expr);
}

}  // namespace kernel::testing::detail

#define KTEST_CHECK_IMPL(expr, fatal)                                                                              \
    ::kernel::testing::detail::run_assert((::kernel::testing::detail::capture_start{} <= expr), (fatal), __FILE__, \
                                          __LINE__, #expr)

#define EXPECT(expr) KTEST_CHECK_IMPL(expr, false)
#define REQUIRE(expr) KTEST_CHECK_IMPL(expr, true)

#else  // CONFIG_KERNEL_TESTING

#define EXPECT(expr) ((void)0)
#define REQUIRE(expr) ((void)0)

#endif  // CONFIG_KERNEL_TESTING
