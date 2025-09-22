#include <kernel/testing/testing.h>
#include <stddef.h>

#include <ktl/string_view>

using namespace kernel::testing;

KTEST(ktl_string_view_default_safe_accessors, "ktl/string_view") {
    ktl::string_view view;

    KTEST_EXPECT_TRUE(view.empty());
    KTEST_EXPECT_TRUE(view.size() == 0);
    KTEST_EXPECT_TRUE(view.data() == nullptr);
    KTEST_EXPECT_TRUE(view.find('a') == ktl::string_view::npos);
}

KTEST(ktl_string_view_find_respects_bounds, "ktl/string_view") {
    ktl::string_view view("safety");

    size_t first = view.find('f');
    size_t missing = view.find('s', view.size());

    KTEST_EXPECT_TRUE(first == 2);
    KTEST_EXPECT_TRUE(missing == ktl::string_view::npos);
}

KTEST(ktl_string_view_rfind_single_character, "ktl/string_view") {
    ktl::string_view view("a");

    size_t index = view.rfind('a');

    KTEST_EXPECT_TRUE(index == 0);
}

KTEST(ktl_string_view_copy_does_not_overrun, "ktl/string_view") {
    ktl::string_view view("kernel");

    char buffer[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    size_t copied = view.copy(buffer, 3);

    KTEST_EXPECT_TRUE(copied == 3);
    KTEST_EXPECT_EQUAL(buffer[0], 'k');
    KTEST_EXPECT_EQUAL(buffer[1], 'e');
    KTEST_EXPECT_EQUAL(buffer[2], 'r');
    KTEST_EXPECT_EQUAL(buffer[3], 'x');

    copied = view.copy(buffer, sizeof(buffer), view.size());
    KTEST_EXPECT_TRUE(copied == 0);
    KTEST_EXPECT_EQUAL(buffer[0], 'k');
}

KTEST(ktl_string_view_substr_clamps_count, "ktl/string_view") {
    ktl::string_view view("archipelago");

    ktl::string_view sub = view.substr(4, 50);

    KTEST_EXPECT_TRUE(sub.size() == 7);
    KTEST_EXPECT_EQUAL(sub[0], 'i');
    KTEST_EXPECT_EQUAL(sub[6], 'o');
}

KTEST(ktl_string_view_starts_with_length_check, "ktl/string_view") {
    ktl::string_view view("arch");

    KTEST_EXPECT_FALSE(view.starts_with("archipelago"));
    KTEST_EXPECT_TRUE(view.starts_with("ar"));
}

KTEST(ktl_string_view_compare_lexicographic, "ktl/string_view") {
    ktl::string_view view("kernel");

    KTEST_EXPECT_EQUAL(view.compare("kernel"), 0);
    KTEST_EXPECT_TRUE(view.compare("kern") > 0);
    KTEST_EXPECT_TRUE(view.compare("kernelz") < 0);
}
