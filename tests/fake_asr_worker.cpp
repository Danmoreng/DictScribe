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
    bool gpu = false;
    bool fail_gpu = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--gpu") {
            gpu = true;
        } else if (argument == "--model" && index + 1 < argc) {
            fail_gpu = std::string(argv[++index]).find("fail-gpu") != std::string::npos;
        }
    }
    if (gpu && fail_gpu) {
        emit({
            {"type", "error"},
            {"code", "TEST_GPU_START_FAILED"},
            {"message", "simulated ASR GPU startup failure"},
            {"recoverable", false},
        });
        return 1;
    }
    emit({{"type", "ready"}, {"engine", "fake-asr"}});
    std::string line;
    int session_number = 0;
    std::string active_language = "de";
    while (std::getline(std::cin, line)) {
        const auto command = nlohmann::json::parse(line);
        const std::string type = command.value("type", "");
        if (type == "start") {
            const std::string language = command.value("language", "");
            if (language != "auto" && language != "de" && language != "en") {
                emit({
                    {"type", "error"},
                    {"code", "WRONG_TEST_LANGUAGE"},
                    {"message", "controller did not forward the selected ASR language"},
                    {"recoverable", false},
                });
                continue;
            }
            ++session_number;
            active_language = language == "auto" ? "de" : language;
            const std::string session = command.value("sessionId", "");
            emit({{"type", "recording_started"}, {"sessionId", session}});
            emit({
                {"type", "audio_level"},
                {"sessionId", session},
                {"rms", 0.18},
                {"peak", 0.72},
            });
            const std::string first = active_language == "en"
                ? "I am testing the local speech cleanup pipeline with several stable words"
                : "Ich teste die lokale Spracherkennung mit mehreren stabilen Wörtern im Diktat";
            const std::string second = first + (active_language == "en"
                ? " and llama_rewriter.cpp" : " und llama_rewriter.cpp");
            const std::string third = second + (active_language == "en" ? " further " : " weiter ") +
                std::to_string(session_number);
            emit({{"type", "transcript_update"}, {"sessionId", session}, {"text", first}});
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            emit({
                {"type", "transcript_update"},
                {"sessionId", session},
                {"text", second},
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            emit({
                {"type", "transcript_update"},
                {"sessionId", session},
                {"text", third},
            });
        } else if (type == "stop") {
            const std::string session = command.value("sessionId", "");
            const std::string final_text = active_language == "en"
                ? "I am testing the local speech cleanup pipeline with several stable words and llama_rewriter.cpp further final " + std::to_string(session_number)
                : "Ich teste die lokale Spracherkennung mit mehreren stabilen Wörtern im Diktat und llama_rewriter.cpp weiter final " + std::to_string(session_number);
            emit({
                {"type", "recording_finalized"},
                {"sessionId", session},
                {"text", final_text},
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
