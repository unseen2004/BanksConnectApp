#include "test_support.h"

namespace testing {
namespace {
std::vector<std::string>& currentFailures() {
    static std::vector<std::string> failures;
    return failures;
}
}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

void recordFailure(const std::string& message) {
    currentFailures().push_back(message);
}

int runAll() {
    int failed = 0;
    for (const TestCase& testCase : registry()) {
        currentFailures().clear();
        try {
            testCase.body();
        } catch (const std::exception& error) {
            recordFailure(std::string("threw std::exception: ") + error.what());
        } catch (...) {
            recordFailure("threw a non-std exception");
        }

        if (currentFailures().empty()) {
            std::cout << "[  ok  ] " << testCase.name << "\n";
        } else {
            ++failed;
            std::cout << "[ FAIL ] " << testCase.name << "\n";
            for (const std::string& failure : currentFailures()) {
                std::cout << "         " << failure << "\n";
            }
        }
    }

    std::cout << "\n" << registry().size() << " tests, " << failed << " failed" << std::endl;
    return failed == 0 ? 0 : 1;
}

}  // namespace testing

int main() {
    return testing::runAll();
}
