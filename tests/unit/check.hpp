#pragma once
// A minimal test framework: no dependencies, one binary, readable output.
//
//     TEST_CASE("json", Json) {
//         Check(json::Parse("{}", &v), "parses an empty object");
//     }

#include <cstdio>
#include <vector>

namespace test {
struct Case {
    const char* name;
    void (*run)();
};

std::vector<Case>& Registry();
int& Failures();

inline void Check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++Failures(); }
}

struct Registrar {
    Registrar(const char* name, void (*run)()) { Registry().push_back({name, run}); }
};
}  // namespace test

#define TEST_CASE(name, function)                                     \
    static void function();                                           \
    static const test::Registrar function##Registrar(name, function); \
    static void function()
