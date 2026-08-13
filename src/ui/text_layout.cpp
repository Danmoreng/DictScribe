#include "ui/text_layout.hpp"

#include <utility>

namespace dictscribe::ui {

std::vector<std::string> SplitExplicitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string line(text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    return lines;
}

} // namespace dictscribe::ui
