#include "llama_rewriter.hpp"

#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class Runtime {
public:
    void emit(nlohmann::json message) {
        message["seq"] = ++sequence_;
        std::cout << dictscribe::protocol::encode(message) << std::flush;
    }

    void emit_error(
        const std::string& code,
        const std::string& message,
        bool recoverable,
        const std::string& id = {},
        int version = dictscribe::protocol::kVersion) {
        auto result = dictscribe::protocol::error(
            code,
            message,
            recoverable,
            id.empty() ? std::nullopt : std::optional{id});
        result["v"] = version;
        emit(std::move(result));
    }

    bool load(const std::string& model_path, int gpu_layers, std::uint32_t context_size) {
        emit({{"v", 1}, {"type", "loading_model"}});
        std::string error;
        if (!rewriter_.load(model_path, gpu_layers, context_size, error)) {
            emit_error("MODEL_LOAD_FAILED", error, false);
            return false;
        }
        emit({
            {"v", 1},
            {"type", "ready"},
            {"engine", "llama.cpp"},
            {"modelArchitecture", rewriter_.model_architecture()},
            {"chatTemplateAvailable", rewriter_.has_chat_template()},
            {"gpuLayers", gpu_layers},
            {"contextSize", context_size},
        });
        return true;
    }

    bool handle(const nlohmann::json& command) {
        const auto type = dictscribe::protocol::require_string(command, "type");
        const auto id = dictscribe::protocol::require_string(command, "id");
        if (type == "ping") {
            emit({{"v", 1}, {"type", "pong"}, {"id", id}});
            return true;
        }
        if (type == "shutdown") {
            emit({
                {"v", 1},
                {"type", "command_ack"},
                {"id", id},
                {"command", "shutdown"},
            });
            emit({{"v", 1}, {"type", "shutdown_complete"}});
            return false;
        }
        if (type == "rewrite_tail") {
            return handle_rewrite_tail(command, id);
        }
        if (type != "rewrite") {
            emit_error("UNKNOWN_COMMAND", "Unknown command type: " + type, true, id);
            return true;
        }

        const auto request_id = dictscribe::protocol::require_string(command, "requestId");
        const auto transcript = dictscribe::protocol::require_string(command, "text");
        const auto source_language = command.value("language", "auto");
        emit({
            {"v", 1},
            {"type", "command_ack"},
            {"id", id},
            {"command", "rewrite"},
            {"requestId", request_id},
        });

        std::string rewritten;
        std::string error;
        if (!rewriter_.rewrite(transcript, source_language, 1024, rewritten, error)) {
            emit_error("REWRITE_FAILED", error, true, id);
            return true;
        }
        emit({
            {"v", 1},
            {"type", "rewrite_completed"},
            {"id", id},
            {"requestId", request_id},
            {"text", rewritten},
        });
        return true;
    }

private:
    bool handle_rewrite_tail(const nlohmann::json& command, const std::string& id) {
        if (command.value("v", 0) != dictscribe::protocol::kRewriteTailVersion) {
            emit_error(
                "UNSUPPORTED_PROTOCOL",
                "rewrite_tail requires protocol version 2",
                true,
                id,
                dictscribe::protocol::kRewriteTailVersion);
            return true;
        }
        const auto require_text = [&](const char* field) {
            if (!command.contains(field) || !command[field].is_string()) {
                throw std::runtime_error(
                    std::string("missing or invalid string field: ") + field);
            }
            return command[field].get<std::string>();
        };
        const auto require_nonnegative_integer = [&](const char* field) -> std::uint64_t {
            if (!command.contains(field) || !command[field].is_number_integer()) {
                throw std::runtime_error(
                    std::string("missing or invalid integer field: ") + field);
            }
            const auto value = command[field].get<std::int64_t>();
            if (value < 0) {
                throw std::runtime_error(
                    std::string("integer field must not be negative: ") + field);
            }
            return static_cast<std::uint64_t>(value);
        };

        const auto request_id = dictscribe::protocol::require_string(command, "requestId");
        const auto session_id = dictscribe::protocol::require_string(command, "sessionId");
        const auto tail_revision = require_nonnegative_integer("tailRevision");
        const auto first_span_id = require_nonnegative_integer("firstStableSpanId");
        const auto last_span_id = require_nonnegative_integer("lastStableSpanId");
        if (last_span_id < first_span_id) {
            throw std::runtime_error("lastStableSpanId must not precede firstStableSpanId");
        }
        dictscribe::rewrite::RewriteTailInput input{
            .language_hint = require_text("languageHint"),
            .read_only_context = require_text("readOnlyContext"),
            .editable_tail = require_text("editableTail"),
            .new_asr_text = require_text("newAsrText"),
        };
        constexpr std::size_t kMaximumFieldBytes = 16 * 1024;
        if (input.language_hint.empty() || input.language_hint.size() > 32 ||
            input.read_only_context.size() > kMaximumFieldBytes ||
            input.editable_tail.size() > kMaximumFieldBytes ||
            input.new_asr_text.size() > kMaximumFieldBytes ||
            (input.editable_tail.empty() && input.new_asr_text.empty())) {
            emit_error(
                "INVALID_REWRITE_TAIL",
                "rewrite_tail fields are empty or exceed their size limit",
                true,
                id,
                dictscribe::protocol::kRewriteTailVersion);
            return true;
        }

        emit({
            {"v", dictscribe::protocol::kRewriteTailVersion},
            {"type", "command_ack"},
            {"id", id},
            {"command", "rewrite_tail"},
            {"requestId", request_id},
        });
        std::string replacement_tail;
        std::string error;
        if (!rewriter_.rewrite_tail(input, 384, replacement_tail, error)) {
            emit_error(
                "REWRITE_TAIL_FAILED",
                error,
                true,
                id,
                dictscribe::protocol::kRewriteTailVersion);
            return true;
        }
        emit({
            {"v", dictscribe::protocol::kRewriteTailVersion},
            {"type", "rewrite_tail_completed"},
            {"id", id},
            {"requestId", request_id},
            {"sessionId", session_id},
            {"tailRevision", tail_revision},
            {"firstStableSpanId", first_span_id},
            {"lastStableSpanId", last_span_id},
            {"replacementTail", replacement_tail},
        });
        return true;
    }

    std::uint64_t sequence_ = 0;
    dictscribe::rewrite::LlamaRewriter rewriter_;
};

} // namespace

int main(int argc, char** argv) {
    std::string model_path;
    bool stdio = false;
    int protocol_version = 1;
    int gpu_layers = 0;
    std::uint32_t context_size = 2048;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--stdio") {
            stdio = true;
        } else if (argument == "--model" && index + 1 < argc) {
            model_path = argv[++index];
        } else if (argument == "--protocol-version" && index + 1 < argc) {
            protocol_version = std::stoi(argv[++index]);
        } else if (argument == "--gpu-layers" && index + 1 < argc) {
            gpu_layers = std::stoi(argv[++index]);
        } else if (argument == "--context-size" && index + 1 < argc) {
            context_size = static_cast<std::uint32_t>(std::stoul(argv[++index]));
        } else if (argument == "--version") {
            std::cout << "dictscribe-rewrite-worker " << DICTSCRIBE_RUNTIME_VERSION << '\n';
            return 0;
        }
    }

    if (!stdio || model_path.empty() ||
        (protocol_version != dictscribe::protocol::kVersion &&
         protocol_version != dictscribe::protocol::kRewriteTailVersion)) {
        std::cerr << "Usage: dictscribe-rewrite-worker --stdio --model MODEL.gguf "
                     "--protocol-version 1|2 [--gpu-layers N] [--context-size N]\n";
        return 2;
    }

    Runtime runtime;
    runtime.emit({
        {"v", 1},
        {"type", "hello"},
        {"runtimeVersion", DICTSCRIBE_RUNTIME_VERSION},
        {"protocolVersions", {1, 2}},
        {"engine", "llama.cpp"},
    });
    if (!runtime.load(model_path, gpu_layers, context_size)) {
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            if (!runtime.handle(dictscribe::protocol::parse(line))) {
                break;
            }
        } catch (const std::exception& exception) {
            std::cerr << "protocol error: " << exception.what() << '\n';
            runtime.emit_error("MALFORMED_COMMAND", exception.what(), true);
        }
    }
    return 0;
}
