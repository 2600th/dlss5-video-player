#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace test_support {

inline int failure_count = 0;
// Name of the case a runner is executing, printed ahead of every failure it
// produces; null outside a named case.
inline const char* current_case = nullptr;

inline std::ostream& failure(std::string_view file, int line)
{
    ++failure_count;
    if (current_case) std::cerr << '[' << current_case << "] ";
    return std::cerr << file << ':' << line << ": ";
}

inline void check(bool condition, std::string_view expression, std::string_view file, int line)
{
    if (!condition) failure(file, line) << "CHECK failed: " << expression << '\n';
}

// A failed CHECK_EQ prints the two values when both insert into a narrow
// stream, since a reader cannot tell an off-by-one from a wrong sign by the
// expressions alone. Wide strings and scoped enums fall back to the
// expressions (C++20 deletes the wide inserters on a narrow stream).
template <typename Value>
concept NarrowPrintable = requires(std::ostream& out, const Value& value) { out << value; };

template <typename Expected, typename Actual>
void check_equal(const Expected& expected, const Actual& actual,
                 std::string_view expected_expression, std::string_view actual_expression,
                 std::string_view file, int line)
{
    if (!(expected == actual)) {
        std::ostream& out = failure(file, line) << "CHECK_EQ failed: "
                                                << expected_expression << " != " << actual_expression;
        if constexpr (NarrowPrintable<Expected> && NarrowPrintable<Actual>)
            out << " (expected " << expected << ", actual " << actual << ')';
        out << '\n';
    }
}

} // namespace test_support

#define CHECK(expression) \
    ::test_support::check((expression), #expression, __FILE__, __LINE__)

#define CHECK_EQ(expected, actual) \
    ::test_support::check_equal((expected), (actual), #expected, #actual, __FILE__, __LINE__)
