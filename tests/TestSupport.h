#pragma once

// excpt.h rather than windows.h: the case runner below needs __except's
// filter constant and GetExceptionCode, and nothing else Windows offers. Every
// target defines NOMINMAX now, but a header this far down should not decide
// what an including test sees of windows.h.
#include <excpt.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace test_support {

inline int failure_count = 0;
// Every CHECK/CHECK_EQ/REQUIRE that was evaluated, passing or not. A suite
// that silently stops asserting - a case that returns early, a split that
// drops a block - keeps a failure count of zero, so the count of assertions
// actually reached is what makes that visible. run_cases fails a case that
// reaches none; the count used to be kept and never read.
inline int assertion_count = 0;
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
    ++assertion_count;
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
    ++assertion_count;
    if (!(expected == actual)) {
        std::ostream& out = failure(file, line) << "CHECK_EQ failed: "
                                                << expected_expression << " != " << actual_expression;
        if constexpr (NarrowPrintable<Expected> && NarrowPrintable<Actual>)
            out << " (expected " << expected << ", actual " << actual << ')';
        out << '\n';
    }
}

// Thrown by a failed REQUIRE and caught by the case runner. REQUIRE has
// already recorded the failure by then, so the runner only has to stop the
// case - see run_case_catching.
//
// Why REQUIRE exists: CHECK is non-fatal, which forced roughly 218
// `CHECK(x.has_value()); if (!x) return;` pairs across the suite. Every one of
// them turns a failure into silently abandoning the rest of its case, and the
// `return` is easy to forget - then the next line dereferences the empty
// optional and takes the process down.
struct RequirementFailed {};

// The REQUIRE macro counts itself when it is evaluated, pass or fail, so a
// case that only REQUIREs is not mistaken for one that asserts nothing.
[[noreturn]] inline void requirement_failed(std::string_view expression,
                                            std::string_view file, int line)
{
    failure(file, line) << "REQUIRE failed: " << expression << '\n';
    throw RequirementFailed{};
}

struct TestCase {
    const char* name;
    void (*run)();
};

struct CaseOutcome {
    unsigned long exceptionCode{};
    std::string exception;
};

struct RunSummary {
    size_t ran{};
    size_t failed{};
};

inline void run_case_catching(void (*run)(), CaseOutcome& outcome)
{
    try {
        run();
    } catch (const RequirementFailed&) {
        // Already counted and printed where it fired. The throw exists to end
        // the case, not to report anything.
    } catch (const std::exception& error) {
        outcome.exception = error.what();
        if (outcome.exception.empty()) outcome.exception = "std::exception";
    } catch (...) {
        outcome.exception = "non-standard exception";
    }
}

// __try cannot share a frame with objects that need unwinding, so the C++
// catch lives one call down. An access violation or a stack overflow in one
// case used to take every case after it, and the failing name, with it.
inline void run_case_guarded(void (*run)(), CaseOutcome& outcome) noexcept
{
    __try {
        run_case_catching(run, outcome);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outcome.exceptionCode = GetExceptionCode();
    }
}

// Runs every case whose name contains `only` (all of them when it is empty),
// attributing each failed CHECK, failed REQUIRE, uncaught exception and
// structured exception to the case it happened in. A case that finishes
// without evaluating a single assertion fails too: it proved nothing, and
// green was the only thing it could ever report.
inline RunSummary run_cases(const TestCase* cases, size_t count, std::string_view only)
{
    RunSummary summary;
    std::vector<const char*> failed;
    for (size_t index = 0; index < count; ++index) {
        const TestCase& test = cases[index];
        if (std::string_view(test.name).find(only) == std::string_view::npos) continue;
        ++summary.ran;
        const int before = failure_count;
        const int assertionsBefore = assertion_count;
        current_case = test.name;
        CaseOutcome outcome;
        run_case_guarded(test.run, outcome);
        current_case = nullptr;
        if (assertion_count == assertionsBefore && outcome.exception.empty() && outcome.exceptionCode == 0) {
            ++failure_count;
            std::cerr << '[' << test.name << "] made no assertions\n";
        }
        if (!outcome.exception.empty()) {
            ++failure_count;
            std::cerr << '[' << test.name << "] uncaught exception: " << outcome.exception << '\n';
        }
        if (outcome.exceptionCode != 0) {
            ++failure_count;
            std::cerr << '[' << test.name << "] structured exception 0x" << std::hex
                      << outcome.exceptionCode << std::dec << '\n';
        }
        if (failure_count != before) failed.push_back(test.name);
    }
    for (const char* name : failed) std::cerr << "FAILED: " << name << '\n';
    summary.failed = failed.size();
    return summary;
}

} // namespace test_support

#define CHECK(expression) \
    ::test_support::check((expression), #expression, __FILE__, __LINE__)

#define CHECK_EQ(expected, actual) \
    ::test_support::check_equal((expected), (actual), #expected, #actual, __FILE__, __LINE__)

// Fatal counterpart to CHECK: records the failure and abandons the case. Use
// it for a precondition the rest of the case would dereference.
#define REQUIRE(expression)                                                   \
    do {                                                                      \
        ++::test_support::assertion_count;                                    \
        if (!(expression))                                                    \
            ::test_support::requirement_failed(#expression, __FILE__, __LINE__); \
    } while (false)

#define TEST_CASE(function) ::test_support::TestCase{#function, &function}
