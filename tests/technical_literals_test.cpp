#include "workers/rewrite/technical_literals.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    const auto protected_text = dictscribe::rewrite::protect_technical_literals(
        "Open README.md and /home/sebastian/file_name.cpp with QVN 3.5-Q8_0.");
    assert(protected_text.literals.size() == 4);
    assert(protected_text.text.find("README.md") == std::string::npos);
    assert(protected_text.text.find("/home/sebastian/file_name.cpp") == std::string::npos);

    std::string output = protected_text.text;
    std::string error;
    assert(dictscribe::rewrite::restore_technical_literals(protected_text, output, error));
    assert(output == "Open README.md and /home/sebastian/file_name.cpp with QVN 3.5-Q8_0.");

    std::string duplicated = protected_text.placeholders.front() + " " +
        protected_text.placeholders.front();
    assert(!dictscribe::rewrite::restore_technical_literals(protected_text, duplicated, error));

    std::string invented = protected_text.placeholders.front() + " /invented/path";
    assert(!dictscribe::rewrite::restore_technical_literals(protected_text, invented, error));

    std::cout << "Technical literal tests passed\n";
    return 0;
}
