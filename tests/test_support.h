#ifndef BANKSCONNECTAPP_TEST_SUPPORT_H
#define BANKSCONNECTAPP_TEST_SUPPORT_H

// A dependency-free test harness: the project has no test framework available, and
// pulling one in would add a network fetch to the build.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();
void recordFailure(const std::string& message);
int runAll();

struct Registrar {
    Registrar(const std::string& name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

template <typename A, typename B>
void expectEq(const A& actual, const B& expected, const char* expression, const char* file, int line) {
    if (!(actual == expected)) {
        std::ostringstream out;
        out << file << ":" << line << ": " << expression << "\n      actual: " << actual
            << "\n    expected: " << expected;
        recordFailure(out.str());
    }
}

inline void expectTrue(bool value, const char* expression, const char* file, int line) {
    if (!value) {
        std::ostringstream out;
        out << file << ":" << line << ": expected true: " << expression;
        recordFailure(out.str());
    }
}

}  // namespace testing

#define TEST(name)                                                             \
    static void name();                                                        \
    static ::testing::Registrar registrar_##name(#name, name);                 \
    static void name()

#define EXPECT_EQ(actual, expected) \
    ::testing::expectEq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define EXPECT_TRUE(value) ::testing::expectTrue((value), #value, __FILE__, __LINE__)
#define EXPECT_FALSE(value) ::testing::expectTrue(!(value), "!(" #value ")", __FILE__, __LINE__)

#endif  // BANKSCONNECTAPP_TEST_SUPPORT_H
