#include <kernel/testing/testing.h>
#include <stddef.h>

#include <ktl/string_view>

using namespace kernel::testing;

KTEST_MODULE("ktl/string_view");

constexpr ktl::string_view compile_time_view("archipelago");
static_assert(compile_time_view.substr(4, 3) == "ipe");
constexpr bool copy_is_constant_evaluated() {
    char out[4]{};
    return compile_time_view.copy(out, 3, 4) == 3 && out[0] == 'i' && out[1] == 'p' && out[2] == 'e';
}
static_assert(copy_is_constant_evaluated());

KTEST_CASE(ktl_string_view_default_safe_accessors) {
    ktl::string_view view;

    KTEST_EXPECT_ALL(view.empty(), view.size() == 0, view.data() == nullptr, view.find('a') == ktl::string_view::npos);
}

KTEST_CASE(ktl_string_view_element_access_and_iteration) {
    ktl::string_view view("kernel");

    KTEST_EXPECT_ALL(view[0] == 'k', view[3] == 'n', view[view.size() - 1] == 'l');

    // Element access stays constant-evaluable.
    static_assert(ktl::string_view("abc")[1] == 'b');

    // begin()/end() drive range-for.
    ktl::string_view sv("abc");
    int n     = 0;
    char last = 0;
    for (char c : sv) {
        last = c;
        ++n;
    }
    KTEST_EXPECT_ALL(n == 3, last == 'c');
}

KTEST_CASE(ktl_string_view_find) {
    ktl::string_view view("safety");

    KTEST_EXPECT_TRUE(view.find('f') == 2);
    // A start position at or past size() finds nothing, even for a present character.
    KTEST_EXPECT_TRUE(view.find('s', view.size()) == ktl::string_view::npos);
    KTEST_EXPECT_TRUE(ktl::string_view("abc").find('z', 0) == ktl::string_view::npos);
}

KTEST_CASE(ktl_string_view_copy_and_substr_clamp) {
    ktl::string_view view("kernel");

    // copy stops at the requested count and never overruns the destination.
    char buffer[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    size_t copied  = view.copy(buffer, 3);

    KTEST_EXPECT_ALL(copied == 3, buffer[0] == 'k', buffer[1] == 'e', buffer[2] == 'r', buffer[3] == 'x');

    // A copy starting at size() writes nothing.
    copied = view.copy(buffer, sizeof(buffer), view.size());
    KTEST_EXPECT_ALL(copied == 0, buffer[0] == 'k');

    // substr clamps an oversized count to the remaining length.
    ktl::string_view sub = ktl::string_view("archipelago").substr(4, 50);
    KTEST_EXPECT_ALL(sub.size() == 7, sub[0] == 'i', sub[6] == 'o');
}

KTEST_CASE(ktl_string_view_compare_and_starts_with) {
    ktl::string_view view("kernel");

    KTEST_EXPECT_ALL(view.compare("kernel") == 0, view.compare("kern") > 0, view.compare("kernelz") < 0);

    // starts_with must reject a prefix longer than the view.
    ktl::string_view arch("arch");
    KTEST_EXPECT_ALL(!arch.starts_with("archipelago"), arch.starts_with("ar"));
}

KTEST_CASE(ktl_string_view_equality) {
    ktl::string_view a("kernel");
    ktl::string_view b("kernel");
    ktl::string_view prefix("kern");

    KTEST_EXPECT_ALL(a == b, a != prefix, a.substr(0, 4) == prefix, ktl::string_view() == ktl::string_view(),
                     ktl::string_view("abc") != "abd");

    // Views over the middle of a buffer are not NUL-terminated; equality must only read size() chars.
    const char buffer[] = "kernelspace";
    ktl::string_view middle(buffer + 3, 3);  // "nel"

    KTEST_EXPECT_ALL(middle == ktl::string_view("nel"), middle != ktl::string_view("nels"));
}
