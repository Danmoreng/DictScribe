#include "runtime_controller.hpp"

#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string model_path;
    bool stdio = false;
    bool use_gpu = false;
    int protocol_version = 1;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--stdio") {
            stdio = true;
        } else if (argument == "--gpu") {
            use_gpu = true;
        } else if (argument == "--model" && index + 1 < argc) {
            model_path = argv[++index];
        } else if (argument == "--protocol-version" && index + 1 < argc) {
            protocol_version = std::stoi(argv[++index]);
        } else if (argument == "--version") {
            std::cout << "dictscribe-asr-worker " << DICTSCRIBE_RUNTIME_VERSION << '\n';
            return 0;
        }
    }

    if (!stdio || model_path.empty() || protocol_version != dictscribe::protocol::kVersion) {
        std::cerr << "Usage: dictscribe-asr-worker --stdio --model MODEL.gguf "
                     "--protocol-version 1 [--gpu]\n";
        return 2;
    }

    dictscribe::asr::RuntimeController controller(
        model_path,
        use_gpu,
        [](const nlohmann::json& message) {
            std::cout << dictscribe::protocol::encode(message) << std::flush;
        });
    controller.emit_hello();
    if (!controller.load_model()) {
        return 1;
    }

    std::string line;
    while (!controller.should_exit() && std::getline(std::cin, line)) {
        try {
            controller.handle(dictscribe::protocol::parse(line));
        } catch (const std::exception& exception) {
            std::cerr << "protocol error: " << exception.what() << '\n';
            std::cout << dictscribe::protocol::encode(dictscribe::protocol::error(
                "MALFORMED_COMMAND", exception.what(), true)) << std::flush;
        }
    }
    controller.shutdown();
    return 0;
}
