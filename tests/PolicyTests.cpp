#include "TestSupport.h"

namespace {

void harness_sanity_test()
{
    CHECK(true);
    CHECK_EQ(2 + 2, 4);
}

} // namespace

int main()
{
    harness_sanity_test();

    if (test_support::failure_count != 0) {
        std::cerr << test_support::failure_count << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "PolicyTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
