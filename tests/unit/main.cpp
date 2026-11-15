// Unit tests for everything that runs without the game. No network access needed.

#include <cstdio>

#include "check.hpp"

namespace test {
std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

int& Failures() {
    static int failures = 0;
    return failures;
}
}  // namespace test

int main() {
    for (const test::Case& c : test::Registry()) {
        printf("%s\n", c.name);
        c.run();
    }

    const int failures = test::Failures();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
