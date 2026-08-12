#pragma once

#include <string>
#include <string_view>

namespace dictscribe::rewrite {

std::string resolve_language_code(std::string_view requested, std::string_view source_text);
std::string language_display_name(std::string_view language_code);
bool output_preserves_language(
    std::string_view language_code,
    std::string_view source_text,
    std::string_view output_text);

} // namespace dictscribe::rewrite
