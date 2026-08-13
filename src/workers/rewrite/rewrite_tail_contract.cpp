#include "rewrite_tail_contract.hpp"

#include <nlohmann/json.hpp>

namespace dictscribe::rewrite {

std::string build_rewrite_tail_model_input(const RewriteTailInput& input) {
    return nlohmann::json{
        {"language_hint", input.language_hint},
        {"read_only_context", input.read_only_context},
        {"editable_tail", input.editable_tail},
        {"new_asr_text", input.new_asr_text},
    }.dump();
}

const char* rewrite_tail_json_grammar() {
    return R"GBNF(
root ::= ws "{" ws "\"replacement_tail\"" ws ":" ws string ws "}" ws
string ::= "\"" chars "\""
chars ::= char*
char ::= [^"\\\x00-\x1F] | "\\" (["\\/bfnrt] | "u" hex hex hex hex)
hex ::= [0-9a-fA-F]
ws ::= [ \t\n\r]*
)GBNF";
}

bool parse_replacement_tail_json(
    std::string_view response,
    bool meaningful_input,
    std::string& replacement_tail,
    std::string& error) {
    replacement_tail.clear();
    error.clear();
    try {
        const auto parsed = nlohmann::json::parse(response);
        if (!parsed.is_object() || parsed.size() != 1 ||
            !parsed.contains("replacement_tail") ||
            !parsed["replacement_tail"].is_string()) {
            error = "rewrite response must contain exactly one replacement_tail string";
            return false;
        }
        replacement_tail = parsed["replacement_tail"].get<std::string>();
    } catch (const nlohmann::json::exception& exception) {
        error = std::string("rewrite response is not valid JSON: ") + exception.what();
        return false;
    }
    if (meaningful_input && replacement_tail.empty()) {
        error = "rewrite response returned an empty tail for non-empty input";
        return false;
    }
    return true;
}

} // namespace dictscribe::rewrite
