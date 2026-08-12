#include "llama_rewriter.hpp"

#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <cstdint>
#include <iostream>
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
        const std::string& id = {}) {
        auto result = dictscribe::protocol::error(
            code,
            message,
            recoverable,
            id.empty() ? std::nullopt : std::optional{id});
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
        if (!rewriter_.rewrite(transcript, source_language, 512, rewritten, error)) {
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
    std::uint64_t sequence_ = 0;
    dictscribe::rewrite::LlamaRewriter rewriter_;
};

} // namespace

int main(int argc, char** argv) {
    std::string model_path;
    bool stdio = false;
    int protocol_version = 1;
    int gpu_layers = 0;
    std::uint32_t context_size = 4096;

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

    if (!stdio || model_path.empty() || protocol_version != dictscribe::protocol::kVersion) {
        std::cerr << "Usage: dictscribe-rewrite-worker --stdio --model MODEL.gguf "
                     "--protocol-version 1 [--gpu-layers N] [--context-size N]\n";
        return 2;
    }

    Runtime runtime;
    runtime.emit({
        {"v", 1},
        {"type", "hello"},
        {"runtimeVersion", DICTSCRIBE_RUNTIME_VERSION},
        {"protocolVersions", {1}},
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
