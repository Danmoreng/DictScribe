#include "dictation_normalizer.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace dictscribe::rewrite {

namespace {

struct Word {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string lower;
};

std::string ascii_lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

std::vector<Word> words(std::string_view text) {
    std::vector<Word> result;
    for (std::size_t index = 0; index < text.size();) {
        const auto value = static_cast<unsigned char>(text[index]);
        if (!std::isalnum(value) && value < 0x80) {
            ++index;
            continue;
        }
        const std::size_t begin = index;
        while (index < text.size()) {
            const auto current = static_cast<unsigned char>(text[index]);
            if (!std::isalnum(current) && current < 0x80) {
                break;
            }
            ++index;
        }
        result.push_back({begin, index, ascii_lower(text.substr(begin, index - begin))});
    }
    return result;
}

bool is_correction_marker(std::string_view word) {
    return word == "nein" || word == "quatsch" || word == "correction" ||
        word == "korrektur" || word == "actually" || word == "sorry";
}

bool is_strong_replacement_marker(std::string_view word) {
    return word == "quatsch" || word == "correction" || word == "korrektur" ||
        word == "actually" || word == "sorry";
}

std::string normalize_corrections(std::string text) {
    text = std::regex_replace(
        text,
        std::regex(
            R"((\b(?:lautet|ist|war)\b)[^.!?\n]*[.!?]\s*\bnein\b\s*,?\s*\bgenauer\s+gesagt\b\s*)",
            std::regex_constants::icase),
        "$1 ");
    for (;;) {
        const auto tokens = words(text);
        bool changed = false;
        for (std::size_t marker = 0; marker < tokens.size(); ++marker) {
            if (!is_correction_marker(tokens[marker].lower) || marker == 0 || marker + 1 >= tokens.size()) {
                continue;
            }

            const std::size_t maximum_repeat = std::min<std::size_t>({5, marker, tokens.size() - marker - 1});
            std::size_t repeated_words = 0;
            for (std::size_t count = maximum_repeat; count > 0; --count) {
                bool matches = true;
                for (std::size_t offset = 0; offset < count; ++offset) {
                    if (tokens[marker - count + offset].lower != tokens[marker + 1 + offset].lower) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    repeated_words = count;
                    break;
                }
            }
            if (repeated_words > 0) {
                text.erase(
                    tokens[marker - repeated_words].begin,
                    tokens[marker].end - tokens[marker - repeated_words].begin);
                changed = true;
                break;
            }

            if (!is_strong_replacement_marker(tokens[marker].lower)) {
                continue;
            }
            std::size_t clause_start = tokens[marker - 1].begin;
            while (clause_start > 0) {
                const char previous = text[clause_start - 1];
                if (previous == '.' || previous == '?' || previous == '!' ||
                    previous == ';' || previous == '\n') {
                    break;
                }
                --clause_start;
            }
            std::size_t first_word = marker;
            while (first_word > 0 && tokens[first_word - 1].begin >= clause_start) {
                --first_word;
            }
            if (tokens[first_word].lower == tokens[marker + 1].lower) {
                text.erase(tokens[first_word].begin, tokens[marker].end - tokens[first_word].begin);
                changed = true;
                break;
            }
        }
        if (!changed) {
            return text;
        }
    }
}

std::string replace_command(
    std::string text,
    const std::string& expression,
    const std::string& replacement) {
    return std::regex_replace(
        text,
        std::regex(expression, std::regex_constants::icase),
        replacement);
}

std::string normalize_commands(std::string text) {
    text = replace_command(
        std::move(text),
        R"(\bdanach[ \t]+(?:also[ \t]+)?anschließend\b)",
        "anschließend");
    text = replace_command(std::move(text), R"(\b(?:neue|neuer|neuen)\s+absatz\b[.,]?)", "\n\n");
    text = replace_command(std::move(text), R"(\bnew\s+paragraph\b[.,]?)", "\n\n");
    text = replace_command(std::move(text), R"(\b(?:neue|neuer|neuen)\s+zeile\b[.,]?)", "\n");
    text = replace_command(std::move(text), R"(\bnew\s+line\b[.,]?)", "\n");
    text = replace_command(std::move(text), R"([ \t]*\bdoppelpunkt\b[.,]?)", ":");
    text = replace_command(std::move(text), R"([ \t]*\bcolon\b[.,]?)", ":");
    text = replace_command(std::move(text), R"((?:Überschrift|überschrift)\s+)", "");

    text = replace_command(std::move(text), R"([ \t]+\b(?:unterstrich|unter[ \t]+strich|underscore)\b[ \t]*)", "_");
    text = replace_command(std::move(text), R"([ \t]+\b(?:bindestrich|hyphen)\b[ \t]*)", "-");
    text = replace_command(std::move(text), R"([ \t]+\b(?:punkt|dot)\b[ \t]*)", ".");
    text = replace_command(std::move(text), R"([ \t]*\bslash\b[ \t]*)", "/");
    text = replace_command(std::move(text), R"(\b(lautet|gesagt|unter|path|at)/)", "$1 /");
    text = replace_command(std::move(text), R"(\.[ \t]+(md|cpp|hpp|json|gguf|txt)\b)", ".$1");

    text = replace_command(std::move(text), R"([ \t]*\berstens\b[ \t]*)", "1. ");
    text = replace_command(std::move(text), R"([ \t]*\bzweitens\b[ \t]*)", "2. ");
    text = replace_command(std::move(text), R"([ \t]*\bdrittens\b[ \t]*)", "3. ");
    text = replace_command(std::move(text), R"([ \t]*\bviertens\b[ \t]*)", "4. ");
    text = replace_command(std::move(text), R"([ \t]*\bfirst(?:ly)?\b[ \t]*)", "1. ");
    text = replace_command(std::move(text), R"([ \t]*\bsecond(?:ly)?\b[ \t]*)", "2. ");
    text = replace_command(std::move(text), R"([ \t]*\bthird(?:ly)?\b[ \t]*)", "3. ");
    text = replace_command(std::move(text), R"([ \t]*\bfourth(?:ly)?\b[ \t]*)", "4. ");
    text = replace_command(std::move(text), R"(([.!?])([1-4]\.))", "$1 $2");

    text = replace_command(std::move(text), R"([ \t]*\n[ \t]*)", "\n");
    text = replace_command(std::move(text), R"([,;][ \t]*\n)", "\n");
    text = replace_command(std::move(text), R"(\n{3,})", "\n\n");
    return text;
}

} // namespace

std::string normalize_spoken_dictation(std::string_view transcript) {
    return normalize_commands(normalize_corrections(std::string(transcript)));
}

} // namespace dictscribe::rewrite
