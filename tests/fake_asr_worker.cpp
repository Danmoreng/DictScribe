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
    emit({{"type", "ready"}, {"engine", "fake-asr"}});
    std::string line;
    int session_number = 0;
    while (std::getline(std::cin, line)) {
        const auto command = nlohmann::json::parse(line);
        const std::string type = command.value("type", "");
        if (type == "start") {
            if (command.value("language", "") != "de") {
                emit({
                    {"type", "error"},
                    {"code", "WRONG_TEST_LANGUAGE"},
                    {"message", "controller did not forward the selected ASR language"},
                    {"recoverable", false},
                });
                continue;
            }
            ++session_number;
            const std::string session = command.value("sessionId", "");
            emit({{"type", "recording_started"}, {"sessionId", session}});
            emit({{"type", "transcript_update"}, {"sessionId", session}, {"text", "Ich äh teste"}});
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            emit({
                {"type", "transcript_update"},
                {"sessionId", session},
                {"text", "Ich äh teste llama_rewriter.cpp"},
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            emit({
                {"type", "transcript_update"},
                {"sessionId", session},
                {"text", "Ich äh teste llama_rewriter.cpp weiter " + std::to_string(session_number)},
            });
        } else if (type == "stop") {
            const std::string session = command.value("sessionId", "");
            emit({
                {"type", "recording_finalized"},
                {"sessionId", session},
                {"text", "Ich teste llama_rewriter.cpp weiter final " + std::to_string(session_number)},
            });
        } else if (type == "cancel") {
            emit({{"type", "recording_cancelled"}, {"sessionId", command.value("sessionId", "")}});
        } else if (type == "shutdown") {
            emit({{"type", "shutdown_complete"}});
            return 0;
        }
    }
    return 0;
}
