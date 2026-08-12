#include "language_guard.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace dictscribe::rewrite {

namespace {

const std::unordered_set<std::string> kGermanMarkers = {
    "aber", "auch", "auf", "aus", "bei", "bin", "bis", "das", "dass", "dem",
    "den", "der", "des", "die", "dies", "doch", "du", "ein", "eine", "einen",
    "einer", "er", "erstens", "es", "für", "gut", "haben", "hat", "heute", "hier",
    "ich", "im", "in", "ist", "kann", "kein", "keine", "man", "mein", "mit",
    "möchte", "nicht", "noch", "oder", "schon", "sehr", "sie", "sind", "sondern",
    "über", "und", "uns", "von", "vor", "war", "was", "wenn", "werden", "wie",
    "wir", "wird", "würde", "zu", "zum", "zur", "zweitens",
};

const std::unordered_set<std::string> kEnglishMarkers = {
    "a", "also", "an", "and", "are", "as", "at", "be", "being", "but", "by",
    "can", "do", "does", "exactly", "first", "for", "fourth", "from", "good", "had",
    "has", "have", "he", "hello", "her", "here", "his", "how", "i", "if", "in",
    "is", "it", "like", "me", "my", "new", "not", "of", "on", "or", "our", "second",
    "she", "so", "that", "the", "their", "them", "then", "they", "third", "this", "to",
    "today", "tomorrow", "want", "was", "we", "well", "what", "when", "where", "which",
    "will", "with", "world", "would", "you", "your",
};

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const unsigned char value : text) {
        if (std::isalnum(value) || value >= 0x80 || value == '\'') {
            current.push_back(value < 0x80 ? static_cast<char>(std::tolower(value)) : static_cast<char>(value));
        } else if (!current.empty()) {
            result.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        result.push_back(std::move(current));
    }
    return result;
}

int score(const std::vector<std::string>& tokens, const std::unordered_set<std::string>& markers) {
    return static_cast<int>(std::count_if(tokens.begin(), tokens.end(), [&](const auto& token) {
        return markers.contains(token);
    }));
}

std::pair<int, int> language_scores(std::string_view text) {
    const auto tokens = words(text);
    return {score(tokens, kGermanMarkers), score(tokens, kEnglishMarkers)};
}

bool clearly_english(std::string_view text) {
    const auto [german, english] = language_scores(text);
    return english >= 4 && english >= german * 2 + 2;
}

bool clearly_german(std::string_view text) {
    const auto [german, english] = language_scores(text);
    return german >= 4 && german >= english * 2 + 2;
}

} // namespace

std::string resolve_language_code(std::string_view requested, std::string_view source_text) {
    std::string normalized(requested);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (normalized == "de" || normalized.starts_with("de-")) return "de";
    if (normalized == "en" || normalized.starts_with("en-")) return "en";
    if (!normalized.empty() && normalized != "auto") return normalized;

    const auto [german, english] = language_scores(source_text);
    if (german >= 3 && german >= english * 2 + 1) return "de";
    if (english >= 3 && english >= german * 2 + 1) return "en";
    return "auto";
}

std::string language_display_name(std::string_view language_code) {
    if (language_code == "de") return "German (de)";
    if (language_code == "en") return "English (en)";
    if (language_code.empty() || language_code == "auto") return "the source language";
    return std::string(language_code);
}

bool output_preserves_language(
    std::string_view language_code,
    std::string_view source_text,
    std::string_view output_text) {
    const std::string resolved = resolve_language_code(language_code, source_text);
    if (resolved == "de") return !clearly_english(output_text);
    if (resolved == "en") return !clearly_german(output_text);
    return true;
}

} // namespace dictscribe::rewrite
