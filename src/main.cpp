#include "rym_crosscheck/cli/application.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include <iostream>
int wmain(int argc, wchar_t *argv[]) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(argc);
        for (int i = 0; i < argc; ++i)
            arguments.push_back(rym::utf8(argv[i]));
        return rym::application(arguments);
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
