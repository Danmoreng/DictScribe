#include "rewrite_tail_contract.hpp"

#include "technical_literals.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>

namespace dictscribe::rewrite {

namespace {

bool valid_utf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7F) {
            ++index;
            continue;
        } else if ((first & 0xE0) == 0xC0) {
            continuation_count = 1;
            code_point = first & 0x1F;
        } else if ((first & 0xF0) == 0xE0) {
            continuation_count = 2;
            code_point = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0) {
            continuation_count = 3;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) return false;
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            code_point = (code_point << 6) | (next & 0x3F);
        }
        const std::uint32_t minimum = continuation_count == 1
            ? 0x80U : continuation_count == 2 ? 0x800U : 0x10000U;
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return character < 0x80
            ? static_cast<char>(std::tolower(character))
            : static_cast<char>(character);
    });
    return value;
}

bool edge_punctuation(unsigned char character) {
    return character < 0x80 &&
        std::ispunct(character) != 0 && character != '_' && character != '-' &&
        character != '.' && character != '/' && character != '\\';
}

std::vector<std::string> normalized_tokens(std::string_view text) {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < text.size();) {
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index])) == 0) {
            ++index;
        }
        std::size_t core_begin = begin;
        std::size_t core_end = index;
        while (core_begin < core_end &&
               edge_punctuation(static_cast<unsigned char>(text[core_begin]))) {
            ++core_begin;
        }
        while (core_end > core_begin &&
               edge_punctuation(static_cast<unsigned char>(text[core_end - 1]))) {
            --core_end;
        }
        if (core_begin < core_end) {
            result.push_back(ascii_lower(
                std::string(text.substr(core_begin, core_end - core_begin))));
        }
    }
    return result;
}

bool repeats_read_only_suffix(
    std::string_view read_only_context,
    std::string_view replacement_tail) {
    const auto context = normalized_tokens(read_only_context);
    const auto output = normalized_tokens(replacement_tail);
    constexpr std::size_t kMinimumRepeatedTokens = 4;
    if (context.size() < kMinimumRepeatedTokens || output.size() < kMinimumRepeatedTokens) {
        return false;
    }
    const std::size_t maximum = std::min({context.size(), output.size(), std::size_t{16}});
    for (std::size_t count = maximum; count >= kMinimumRepeatedTokens; --count) {
        if (std::equal(
                context.end() - static_cast<std::ptrdiff_t>(count),
                context.end(),
                output.begin())) {
            return true;
        }
    }
    return false;
}

bool ordered_list_number(std::string_view text, std::size_t begin, std::size_t end) {
    std::size_t line_start = begin;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }
    const bool at_line_start = std::all_of(
        text.begin() + static_cast<std::ptrdiff_t>(line_start),
        text.begin() + static_cast<std::ptrdiff_t>(begin),
        [](char character) { return character == ' ' || character == '\t'; });
    return at_line_start && end + 1 < text.size() && text[end] == '.' && text[end + 1] == ' ';
}

std::map<std::string, std::size_t> numeric_anchors(
    std::string_view text,
    bool ignore_ordered_list_numbers) {
    std::map<std::string, std::size_t> result;
    for (std::size_t index = 0; index < text.size();) {
        if (std::isdigit(static_cast<unsigned char>(text[index])) == 0) {
            ++index;
            continue;
        }
        const std::size_t begin = index;
        while (index < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        if (!ignore_ordered_list_numbers || !ordered_list_number(text, begin, index)) {
            ++result[std::string(text.substr(begin, index - begin))];
        }
    }
    return result;
}

std::set<std::string> anchor_components(std::string_view text) {
    std::set<std::string> result;
    for (std::size_t index = 0; index < text.size();) {
        while (index < text.size() &&
               std::isalnum(static_cast<unsigned char>(text[index])) == 0 &&
               static_cast<unsigned char>(text[index]) < 0x80) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size()) {
            const auto character = static_cast<unsigned char>(text[index]);
            if (character < 0x80 && std::isalnum(character) == 0) break;
            ++index;
        }
        if (begin < index) {
            result.insert(ascii_lower(std::string(text.substr(begin, index - begin))));
        }
    }
    return result;
}

bool valid_technical_anchors(
    std::string_view editable_input,
    std::string_view replacement_tail,
    std::string& error) {
    const auto source = protect_technical_literals(editable_input);
    const auto output = protect_technical_literals(replacement_tail);
    std::map<std::string, std::size_t> source_counts;
    std::map<std::string, std::size_t> output_counts;
    for (const auto& literal : source.literals) ++source_counts[ascii_lower(literal)];
    for (const auto& literal : output.literals) ++output_counts[ascii_lower(literal)];
    const auto components = anchor_components(editable_input);

    for (const auto& [literal, count] : output_counts) {
        const auto source_match = source_counts.find(literal);
        if (source_match != source_counts.end()) {
            if (count > source_match->second) {
                error = "rewrite duplicated a technical anchor";
                return false;
            }
            continue;
        }
        if (count > 1) {
            error = "rewrite duplicated a reconstructed technical anchor";
            return false;
        }
        for (const auto& component : anchor_components(literal)) {
            if (!components.contains(component)) {
                error = "rewrite introduced a technical anchor component absent from editable input";
                return false;
            }
        }
    }
    return true;
}

} // namespace

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

bool validate_replacement_tail(
    const RewriteTailInput& input,
    std::string_view replacement_tail,
    std::string& error) {
    error.clear();
    if (!valid_utf8(replacement_tail)) {
        error = "rewrite response is not valid UTF-8";
        return false;
    }
    if (replacement_tail.find("<think>") != std::string_view::npos ||
        replacement_tail.find("</think>") != std::string_view::npos ||
        replacement_tail.find("```") != std::string_view::npos) {
        error = "rewrite response contains model commentary or a code fence";
        return false;
    }
    if (repeats_read_only_suffix(input.read_only_context, replacement_tail)) {
        error = "rewrite response repeats read-only context";
        return false;
    }

    const std::string editable_input = input.editable_tail + " " + input.new_asr_text;
    const auto source_numbers = numeric_anchors(editable_input, false);
    const auto output_numbers = numeric_anchors(replacement_tail, true);
    for (const auto& [number, count] : output_numbers) {
        const auto source = source_numbers.find(number);
        if (source == source_numbers.end() || count > source->second) {
            error = "rewrite introduced or duplicated a numeric anchor";
            return false;
        }
    }
    return valid_technical_anchors(editable_input, replacement_tail, error);
}

} // namespace dictscribe::rewrite
