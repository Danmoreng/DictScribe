#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dictscribe::rewrite {

struct ProtectedTranscript {
    std::string text;
    std::vector<std::string> placeholders;
    std::vector<std::string> literals;
};

std::string technical_literal_placeholder(std::size_t index);
ProtectedTranscript protect_technical_literals(std::string_view transcript);
bool restore_technical_literals(
    const ProtectedTranscript& protected_transcript,
    std::string& output,
    std::string& error);

} // namespace dictscribe::rewrite
