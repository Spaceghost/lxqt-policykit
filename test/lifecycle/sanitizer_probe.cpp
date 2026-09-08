// SPDX-License-Identifier: LGPL-2.1-or-later
// Deliberately invalid programs used only to verify sanitizer enforcement.
#include <cstdio>
#include <cstring>
#include <limits>

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    if (std::strcmp(argv[1], "undefined") == 0)
    {
        volatile int increment = argc;
        int value = std::numeric_limits<int>::max();
        value += increment;
        std::printf("%d\n", value);
    }
    else if (std::strcmp(argv[1], "address") == 0)
    {
        auto *value = new int(argc);
        delete value;
        // Force an observable access; this program is expected to fail.
        volatile int observed = *value;
        std::printf("%d\n", observed);
    }
    else
        return 2;
    std::puts("SANITIZER_DID_NOT_STOP");
    return 0;
}
