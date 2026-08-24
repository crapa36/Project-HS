#include "gameplay_test_support.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gameplay_test
{

void RunGameplayContentTests();
void RunGameplayCombatTests();
void RunGameplayProgressionTests();
void RunGameplayPresentationTests();
void RunGameplayDeterminismTests();

} // namespace gameplay_test

using namespace gameplay_test;

int main(int argc, char **argv)
{
    try
    {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        const auto run = [&](std::string_view expected) {
            return group == "all" || group == expected;
        };
        if (run("content"))
            RunGameplayContentTests();
        if (run("combat"))
            RunGameplayCombatTests();
        if (run("progression"))
            RunGameplayProgressionTests();
        if (run("presentation"))
            RunGameplayPresentationTests();
        if (run("determinism"))
            RunGameplayDeterminismTests();
        Check(group == "all" || group == "content" || group == "combat" ||
                  group == "progression" || group == "presentation" ||
                  group == "determinism",
              "unknown gameplay test group");
        std::cout << "gameplay_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "gameplay_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
