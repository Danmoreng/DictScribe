#pragma once

#include <string>
#include <string_view>

namespace dictscribe::rewrite {

struct RewriteTailInput {
    std::string language_hint;
    std::string read_only_context;
    std::string editable_tail;
    std::string new_asr_text;
};

[[nodiscard]] std::string build_rewrite_tail_model_input(const RewriteTailInput& input);
[[nodiscard]] const char* rewrite_tail_json_grammar();
bool parse_replacement_tail_json(
    std::string_view response,
    bool meaningful_input,
    std::string& replacement_tail,
    std::string& error);
bool validate_replacement_tail(
    const RewriteTailInput& input,
    std::string_view replacement_tail,
    std::string& error);

} // namespace dictscribe::rewrite
