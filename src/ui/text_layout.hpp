#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dictscribe::ui {

std::vector<std::string> SplitExplicitLines(std::string_view text);

} // namespace dictscribe::ui
