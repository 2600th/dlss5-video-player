#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace test_support {

inline int failure_count = 0;

inline void check(bool condition, std::string_view expression, std::string_view file, int line)
{
    if (!condition) {
        ++failure_count;
        std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
    }
}

template <typename Expected, typename Actual>
void check_equal(const Expected& expected, const Actual& actual,
                 std::string_view expected_expression, std::string_view actual_expression,
                 std::string_view file, int line)
{
    if (!(expected == actual)) {
        ++failure_count;
        std::cerr << file << ':' << line << ": CHECK_EQ failed: "
                  << expected_expression << " != " << actual_expression << '\n';
    }
}

} // namespace test_support

#define CHECK(expression) \
    ::test_support::check((expression), #expression, __FILE__, __LINE__)

#define CHECK_EQ(expected, actual) \
    ::test_support::check_equal((expected), (actual), #expected, #actual, __FILE__, __LINE__)
