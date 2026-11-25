#include <ktl/maybe>

// expected-note@ktl/maybe:* 2 {{'maybe' has been explicitly marked deleted here}}
void bind_lvalues(int& mutable_value, const int& const_value) {
    ktl::maybe<int&> mutable_ref{mutable_value};
    ktl::maybe<const int&> const_ref{const_value};
}

void reject_rvalues(int& value) {
    ktl::maybe<int&> mutable_ref{ktl::move(value)};  // expected-error {{call to deleted constructor}}
    ktl::maybe<const int&> const_ref{42};            // expected-error {{call to deleted constructor}}
}
