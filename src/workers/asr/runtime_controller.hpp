#pragma once

#include "audio_capture.hpp"
#include "transcription_engine.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace dictscribe::asr {

class RuntimeController {
public:
    using Writer = std::function<void(const nlohmann::json&)>;

    RuntimeController(std::string model_path, bool use_gpu, Writer writer);
    ~RuntimeController();

    void emit_hello();
    bool load_model();
    void handle(const nlohmann::json& command);
    void shutdown();
    [[nodiscard]] bool should_exit() const { return should_exit_; }

private:
    enum class State { Loading, Ready, Recording, Finalizing, ShuttingDown, Exited };

    void emit(nlohmann::json message);
    void emit_error(
        const std::string& code,
        const std::string& message,
        bool recoverable,
        const std::string& id = {},
        const std::string& session_id = {});
    void acknowledge(const nlohmann::json& command);
    void start(const nlohmann::json& command);
    void stop(const nlohmann::json& command, bool cancelled);
    void worker_loop(std::string session_id);
    void meter_loop(std::string session_id);
    void emit_feed(const FeedResult& result, const std::string& session_id);
    void join_worker();
    void join_meter();

    std::string model_path_;
    bool use_gpu_ = false;
    Writer writer_;
    std::atomic<bool> should_exit_{false};
    std::uint64_t sequence_ = 0;
    std::mutex output_mutex_;
    std::string transcript_;
    State state_ = State::Loading;
    std::string session_id_;
    std::mutex state_mutex_;
    std::thread worker_;
    std::thread meter_;
    AudioRingBuffer ring_{16000 * 10};
    AudioCapture capture_{ring_};
    TranscriptionEngine engine_;
};

} // namespace dictscribe::asr
