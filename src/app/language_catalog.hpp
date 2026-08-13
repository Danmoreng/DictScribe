#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace dictscribe::app {

struct LanguageOption {
    std::string_view code;
    std::string_view label;
};

// Nemotron 3.5 ASR Streaming locales that work without additional fine-tuning.
inline constexpr std::array<LanguageOption, 33> kLanguageOptions = {{
    {"auto", "Automatic detection"},
    {"ar-AR", "Arabic (ar-AR)"},
    {"bg-BG", "Bulgarian (bg-BG)"},
    {"zh-CN", "Chinese, Mandarin (zh-CN)"},
    {"hr-HR", "Croatian (hr-HR)"},
    {"cs-CZ", "Czech (cs-CZ)"},
    {"da-DK", "Danish (da-DK)"},
    {"nl-NL", "Dutch (nl-NL)"},
    {"en-GB", "English, United Kingdom (en-GB)"},
    {"en-US", "English, United States (en-US)"},
    {"et-EE", "Estonian (et-EE)"},
    {"fi-FI", "Finnish (fi-FI)"},
    {"fr-CA", "French, Canada (fr-CA)"},
    {"fr-FR", "French, France (fr-FR)"},
    {"de-DE", "German (de-DE)"},
    {"hi-IN", "Hindi (hi-IN)"},
    {"hu-HU", "Hungarian (hu-HU)"},
    {"it-IT", "Italian (it-IT)"},
    {"ja-JP", "Japanese (ja-JP)"},
    {"ko-KR", "Korean (ko-KR)"},
    {"nb-NO", "Norwegian Bokmal (nb-NO)"},
    {"pl-PL", "Polish (pl-PL)"},
    {"pt-BR", "Portuguese, Brazil (pt-BR)"},
    {"pt-PT", "Portuguese, Portugal (pt-PT)"},
    {"ro-RO", "Romanian (ro-RO)"},
    {"ru-RU", "Russian (ru-RU)"},
    {"sk-SK", "Slovak (sk-SK)"},
    {"es-ES", "Spanish, Spain (es-ES)"},
    {"es-US", "Spanish, United States (es-US)"},
    {"sv-SE", "Swedish (sv-SE)"},
    {"tr-TR", "Turkish (tr-TR)"},
    {"uk-UA", "Ukrainian (uk-UA)"},
    {"vi-VN", "Vietnamese (vi-VN)"},
}};

inline std::string CanonicalLanguageCode(std::string_view value) {
    std::string normalized(value);
    for (char& character : normalized) {
        if (character == '_') character = '-';
        else character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }

    if (normalized == "de") normalized = "de-de";
    if (normalized == "en") normalized = "en-us";
    for (const LanguageOption& option : kLanguageOptions) {
        std::string candidate(option.code);
        for (char& character : candidate) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        if (normalized == candidate) return std::string(option.code);
    }
    return {};
}

inline bool IsSupportedLanguageCode(std::string_view value) {
    return !CanonicalLanguageCode(value).empty();
}

inline std::size_t LanguageOptionIndex(std::string_view value) {
    const std::string canonical = CanonicalLanguageCode(value);
    for (std::size_t index = 0; index < kLanguageOptions.size(); ++index) {
        if (kLanguageOptions[index].code == canonical) return index;
    }
    return 0;
}

inline std::string_view LanguageDisplayName(std::string_view value) {
    return kLanguageOptions[LanguageOptionIndex(value)].label;
}

inline std::string LanguageBadge(std::string_view value) {
    const std::string canonical = CanonicalLanguageCode(value);
    if (canonical.empty() || canonical == "auto") return "AUTO";
    std::string badge = canonical;
    for (char& character : badge) {
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
    }
    return badge;
}

} // namespace dictscribe::app
