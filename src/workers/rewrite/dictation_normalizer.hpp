#pragma once

#include <string>
#include <string_view>

namespace dictscribe::rewrite {

std::string normalize_spoken_dictation(std::string_view transcript);

} // namespace dictscribe::rewrite
