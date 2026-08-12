#include "technical_literals.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace dictscribe::rewrite {

namespace {

constexpr std::string_view kPlaceholderPrefix = "__DICTSCRIBE_LITERAL_";

bool is_edge_punctuation(char value) {
    switch (value) {
    case '"':
    case '\'':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
        return true;
    default:
        return false;
    }
}

bool has_joined_symbol(std::string_view token, char symbol) {
    const auto position = token.find(symbol);
    if (position == std::string_view::npos) {
        return false;
    }
    const auto is_alnum = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) != 0;
    };
    return position > 0 && position + 1 < token.size() &&
        is_alnum(token[position - 1]) && is_alnum(token[position + 1]);
}

bool is_uppercase_identifier(std::string_view token) {
    std::size_t letters = 0;
    for (const unsigned char value : token) {
        if (!std::isalpha(value)) {
            continue;
        }
        ++letters;
        if (!std::isupper(value)) {
            return false;
        }
    }
    return letters >= 2;
}

bool is_technical_literal(std::string_view token) {
    return token.find('/') != std::string_view::npos ||
        token.find('\\') != std::string_view::npos ||
        token.find('_') != std::string_view::npos ||
        has_joined_symbol(token, '.') || has_joined_symbol(token, '-') ||
        is_uppercase_identifier(token);
}

} // namespace

ProtectedTranscript protect_technical_literals(std::string_view transcript) {
    ProtectedTranscript result;
    result.text.reserve(transcript.size());
    for (std::size_t index = 0; index < transcript.size();) {
        if (std::isspace(static_cast<unsigned char>(transcript[index]))) {
            result.text.push_back(transcript[index++]);
            continue;
        }

        const std::size_t token_begin = index;
        while (index < transcript.size() &&
               !std::isspace(static_cast<unsigned char>(transcript[index]))) {
            ++index;
        }
        const std::size_t token_end = index;
        std::size_t core_begin = token_begin;
        std::size_t core_end = token_end;
        while (core_begin < core_end && is_edge_punctuation(transcript[core_begin])) {
            ++core_begin;
        }
        while (core_end > core_begin && is_edge_punctuation(transcript[core_end - 1])) {
            --core_end;
        }
        if (core_end > core_begin && transcript[core_end - 1] == '.' &&
            is_technical_literal(transcript.substr(core_begin, core_end - core_begin - 1))) {
            --core_end;
        }
        const auto core = transcript.substr(core_begin, core_end - core_begin);
        if (!is_technical_literal(core)) {
            result.text.append(transcript.substr(token_begin, token_end - token_begin));
            continue;
        }

        const std::string placeholder = std::string(kPlaceholderPrefix) +
            std::to_string(result.literals.size()) + "__";
        result.text.append(transcript.substr(token_begin, core_begin - token_begin));
        result.text.append(placeholder);
        result.text.append(transcript.substr(core_end, token_end - core_end));
        result.placeholders.push_back(placeholder);
        result.literals.emplace_back(core);
    }
    return result;
}

bool restore_technical_literals(
    const ProtectedTranscript& protected_transcript,
    std::string& output,
    std::string& error) {
    for (std::size_t index = 0; index < protected_transcript.placeholders.size(); ++index) {
        const auto& placeholder = protected_transcript.placeholders[index];
        const auto position = output.find(placeholder);
        if (position == std::string::npos) {
            continue;
        }
        if (output.find(placeholder, position + placeholder.size()) != std::string::npos) {
            output.clear();
            error = "rewrite duplicated a protected technical literal";
            return false;
        }
        output.replace(position, placeholder.size(), protected_transcript.literals[index]);
    }
    if (output.find(kPlaceholderPrefix) != std::string::npos) {
        output.clear();
        error = "rewrite altered a protected technical literal";
        return false;
    }
    const auto observed = protect_technical_literals(output);
    for (const auto& literal : observed.literals) {
        if (std::find(
                protected_transcript.literals.begin(),
                protected_transcript.literals.end(),
                literal) == protected_transcript.literals.end()) {
            output.clear();
            error = "rewrite introduced a new technical literal";
            return false;
        }
    }
    return true;
}

} // namespace dictscribe::rewrite
