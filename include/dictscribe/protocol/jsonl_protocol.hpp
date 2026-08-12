#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace dictscribe::protocol {

inline constexpr int kVersion = 1;
inline constexpr std::size_t kMaximumLineBytes = 1024 * 1024;

nlohmann::json parse(std::string_view line);
std::string encode(const nlohmann::json& message);

nlohmann::json error(
    std::string code,
    std::string message,
    bool recoverable,
    std::optional<std::string> id = std::nullopt,
    std::optional<std::string> session_id = std::nullopt);

std::string require_string(const nlohmann::json& message, std::string_view field);

} // namespace dictscribe::protocol
