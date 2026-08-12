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

int main() {
    emit({{"type", "ready"}, {"engine", "fake-rewrite"}});
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto command = nlohmann::json::parse(line);
        const std::string type = command.value("type", "");
        if (type == "rewrite") {
            const std::string request = command.value("requestId", "");
            emit({{"type", "command_ack"}, {"requestId", request}});
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
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
