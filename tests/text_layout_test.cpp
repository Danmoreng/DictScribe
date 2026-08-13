#include "ui/text_layout.hpp"

#include <cassert>
#include <iostream>
#include <vector>

int main() {
    using dictscribe::ui::SplitExplicitLines;

    assert(SplitExplicitLines("") == std::vector<std::string>{""});
    assert(SplitExplicitLines("One line") == std::vector<std::string>{"One line"});
    assert((SplitExplicitLines("Heading\n\n- First\n- Second") ==
        std::vector<std::string>{"Heading", "", "- First", "- Second"}));
    assert((SplitExplicitLines("First\r\nSecond\r\n") ==
        std::vector<std::string>{"First", "Second", ""}));

    std::cout << "Text layout tests passed\n";
    return 0;
}
