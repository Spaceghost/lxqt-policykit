// SPDX-License-Identifier: LGPL-2.1-or-later
#include "agent_fixture.h"
#include "review_cases.h"
#include "lifecycle_cases.h"
#include "attempt_cases.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    CHECK(argc == 2);
    const std::string name(argv[1]);
    if (!AttemptCases::run(name) && !Review::run(name) && !LifecycleCases::run(name))
    {
        std::cerr << "Unknown scenario: " << name << '\n';
        return 2;
    }
    pump();
    std::cout << "PASS: " << name << '\n';
    return 0;
}
