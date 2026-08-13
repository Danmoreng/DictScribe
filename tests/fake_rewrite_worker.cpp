#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

void emit(nlohmann::json message) {
    message["v"] = 1;
    std::cout << message.dump() << '\n' << std::flush;
}

} // namespace

int main(int argc, char** argv) {
    bool fail_load = false;
    bool fail_rewrite = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--model" && index + 1 < argc) {
            const std::string model = argv[++index];
            fail_load = model.find("fail-load") != std::string::npos;
            fail_rewrite = model.find("fail-rewrite") != std::string::npos;
        }
    }
    if (fail_load) {
        emit({
            {"type", "error"},
            {"code", "MODEL_LOAD_FAILED"},
            {"message", "simulated rewrite model load failure"},
            {"recoverable", false},
        });
        return 1;
    }
    emit({{"type", "ready"}, {"engine", "fake-rewrite"}});
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto command = nlohmann::json::parse(line);
        const std::string type = command.value("type", "");
        if (type == "rewrite") {
            const std::string request = command.value("requestId", "");
            emit({{"type", "command_ack"}, {"requestId", request}});
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            if (fail_rewrite) {
                emit({
                    {"type", "error"},
                    {"code", "REWRITE_FAILED"},
                    {"message", "simulated rewrite failure"},
                    {"recoverable", true},
                });
                continue;
            }
            emit({
                {"type", "rewrite_completed"},
                {"id", command.value("id", "")},
                {"requestId", request},
                {"text", command.value("language", "") + ":" + command.value("text", "")},
            });
        } else if (type == "shutdown") {
            emit({{"type", "shutdown_complete"}});
            return 0;
        }
    }
    return 0;
}
