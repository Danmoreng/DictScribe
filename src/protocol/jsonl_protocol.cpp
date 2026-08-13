#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <stdexcept>

namespace dictscribe::protocol {

nlohmann::json parse(std::string_view line) {
    if (line.empty()) {
        throw std::runtime_error("protocol line is empty");
    }
    if (line.size() > kMaximumLineBytes) {
        throw std::runtime_error("protocol line exceeds the 1 MiB limit");
    }

    auto message = nlohmann::json::parse(line);
    if (!message.is_object()) {
        throw std::runtime_error("protocol message must be a JSON object");
    }
    if (!message.contains("v") || !message["v"].is_number_integer() ||
        (message["v"].get<int>() != kVersion &&
         message["v"].get<int>() != kRewriteTailVersion)) {
        throw std::runtime_error("unsupported or missing protocol version");
    }
    (void)require_string(message, "type");
    return message;
}

std::string encode(const nlohmann::json& message) {
    return message.dump() + '\n';
}

nlohmann::json error(
    std::string code,
    std::string message,
    bool recoverable,
    std::optional<std::string> id,
    std::optional<std::string> session_id) {
    nlohmann::json result = {
        {"v", kVersion},
        {"type", "error"},
        {"code", std::move(code)},
        {"message", std::move(message)},
        {"recoverable", recoverable},
    };
    if (id) {
        result["id"] = std::move(*id);
    }
    if (session_id) {
        result["sessionId"] = std::move(*session_id);
    }
    return result;
}

std::string require_string(const nlohmann::json& message, std::string_view field) {
    const std::string key(field);
    if (!message.contains(key) || !message[key].is_string()) {
        throw std::runtime_error("missing or invalid string field: " + key);
    }
    auto value = message[key].get<std::string>();
    if (value.empty()) {
        throw std::runtime_error("string field must not be empty: " + key);
    }
    return value;
}

} // namespace dictscribe::protocol
