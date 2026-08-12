#include "runtime_controller.hpp"

#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace dictscribe::asr {

RuntimeController::RuntimeController(std::string model_path, bool use_gpu, Writer writer)
    : model_path_(std::move(model_path)), use_gpu_(use_gpu), writer_(std::move(writer)) {}

RuntimeController::~RuntimeController() {
    shutdown();
}

void RuntimeController::emit_hello() {
    emit({
        {"v", protocol::kVersion},
        {"type", "hello"},
        {"runtimeVersion", DICTSCRIBE_RUNTIME_VERSION},
        {"protocolVersions", {protocol::kVersion}},
        {"engine", "nemo-speech.cpp"},
    });
}

bool RuntimeController::load_model() {
    emit({{"v", protocol::kVersion}, {"type", "loading_model"}});
    std::string error;
    if (!engine_.load(model_path_, use_gpu_, error)) {
        emit_error("MODEL_LOAD_FAILED", error, false);
        should_exit_ = true;
        state_ = State::Exited;
        return false;
    }
    state_ = State::Ready;
    emit({
        {"v", protocol::kVersion},
        {"type", "ready"},
        {"engine", "nemo-speech.cpp"},
        {"engineVersion", engine_.version()},
        {"backend", use_gpu_ ? "cuda" : "cpu"},
    });
    return true;
}

void RuntimeController::handle(const nlohmann::json& command) {
    const auto type = protocol::require_string(command, "type");
    if (type == "ping") {
        emit({
            {"v", protocol::kVersion},
            {"type", "pong"},
            {"id", protocol::require_string(command, "id")},
        });
    } else if (type == "start") {
        start(command);
    } else if (type == "stop") {
        stop(command, false);
    } else if (type == "cancel") {
        stop(command, true);
    } else if (type == "shutdown") {
        acknowledge(command);
        shutdown();
    } else {
        emit_error(
            "UNKNOWN_COMMAND",
            "Unknown command type: " + type,
            true,
            command.value("id", ""));
    }
}

void RuntimeController::shutdown() {
    {
        std::lock_guard lock(state_mutex_);
        if (state_ == State::Exited) {
            return;
        }
        state_ = State::ShuttingDown;
        capture_.stop();
    }
    join_worker();
    join_meter();
    engine_.cancel();
    state_ = State::Exited;
    should_exit_ = true;
    emit({{"v", protocol::kVersion}, {"type", "shutdown_complete"}});
}

void RuntimeController::emit(nlohmann::json message) {
    std::lock_guard lock(output_mutex_);
    message["seq"] = ++sequence_;
    writer_(message);
}

void RuntimeController::emit_error(
    const std::string& code,
    const std::string& message,
    bool recoverable,
    const std::string& id,
    const std::string& session_id) {
    auto result = protocol::error(
        code,
        message,
        recoverable,
        id.empty() ? std::nullopt : std::optional{id},
        session_id.empty() ? std::nullopt : std::optional{session_id});
    emit(std::move(result));
}

void RuntimeController::acknowledge(const nlohmann::json& command) {
    nlohmann::json message = {
        {"v", protocol::kVersion},
        {"type", "command_ack"},
        {"id", protocol::require_string(command, "id")},
        {"command", protocol::require_string(command, "type")},
    };
    if (command.contains("sessionId")) {
        message["sessionId"] = protocol::require_string(command, "sessionId");
    }
    emit(std::move(message));
}

void RuntimeController::start(const nlohmann::json& command) {
    const auto id = protocol::require_string(command, "id");
    const auto session_id = protocol::require_string(command, "sessionId");
    const auto language = protocol::require_string(command, "language");

    std::lock_guard lock(state_mutex_);
    if (state_ != State::Ready) {
        emit_error("INVALID_STATE", "start is only valid in ready state", true, id, session_id);
        return;
    }

    std::string error;
    if (!engine_.begin(language, error)) {
        emit_error("TRANSCRIPTION_FAILED", error, true, id, session_id);
        return;
    }

    ring_.clear();
    transcript_.clear();
    session_id_ = session_id;
    acknowledge(command);
    if (!capture_.start(error)) {
        engine_.cancel();
        session_id_.clear();
        emit_error("MICROPHONE_UNAVAILABLE", error, true, {}, session_id);
        return;
    }

    state_ = State::Recording;
    emit({
        {"v", protocol::kVersion},
        {"type", "recording_started"},
        {"sessionId", session_id_},
        {"language", language},
        {"audioDevice", "Default"},
    });
    worker_ = std::thread(&RuntimeController::worker_loop, this, session_id_);
    meter_ = std::thread(&RuntimeController::meter_loop, this, session_id_);
}

void RuntimeController::stop(const nlohmann::json& command, bool cancelled) {
    const auto id = protocol::require_string(command, "id");
    const auto session_id = protocol::require_string(command, "sessionId");
    {
        std::lock_guard lock(state_mutex_);
        if (state_ != State::Recording || session_id_ != session_id) {
            emit_error(
                "INVALID_STATE",
                "stop/cancel does not match the active recording",
                true,
                id,
                session_id);
            return;
        }
        state_ = State::Finalizing;
        capture_.stop();
    }

    acknowledge(command);
    join_worker();
    join_meter();
    if (cancelled) {
        engine_.cancel();
        emit({
            {"v", protocol::kVersion},
            {"type", "recording_cancelled"},
            {"sessionId", session_id},
        });
    } else {
        FeedResult result;
        std::string error;
        if (!engine_.finalize(result, error)) {
            emit_error("TRANSCRIPTION_FAILED", error, true, {}, session_id);
        } else {
            emit_feed(result, session_id);
            emit({
                {"v", protocol::kVersion},
                {"type", "recording_finalized"},
                {"sessionId", session_id},
                {"text", transcript_},
            });
        }
    }

    std::lock_guard lock(state_mutex_);
    session_id_.clear();
    state_ = State::Ready;
}

void RuntimeController::meter_loop(std::string session_id) {
    while (capture_.running()) {
        emit({
            {"v", protocol::kVersion},
            {"type", "audio_level"},
            {"sessionId", session_id},
            {"rms", capture_.rms_level()},
            {"peak", capture_.peak_level()},
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void RuntimeController::worker_loop(std::string session_id) {
    std::vector<float> block(1600);
    bool overflow_reported = false;
    while (capture_.running() || ring_.available() > 0) {
        const auto count = ring_.pop(block.data(), block.size());
        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        FeedResult result;
        std::string error;
        if (!engine_.feed(block.data(), static_cast<int>(count), result, error)) {
            emit_error("TRANSCRIPTION_FAILED", error, true, {}, session_id);
            capture_.stop();
            return;
        }
        emit_feed(result, session_id);

        if (!overflow_reported && ring_.dropped_samples() > 0) {
            overflow_reported = true;
            emit({
                {"v", protocol::kVersion},
                {"type", "warning"},
                {"sessionId", session_id},
                {"code", "AUDIO_BUFFER_OVERFLOW"},
                {"message", "Audio frames were dropped because inference fell behind."},
                {"recoverable", true},
            });
        }
    }
}

void RuntimeController::emit_feed(const FeedResult& result, const std::string& session_id) {
    if (result.text) {
        transcript_ = *result.text;
        emit({
            {"v", protocol::kVersion},
            {"type", "transcript_update"},
            {"sessionId", session_id},
            {"text", transcript_},
            {"final", result.final},
            {"audioProcessedSec", result.audio_processed_seconds},
        });
    }
}

void RuntimeController::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void RuntimeController::join_meter() {
    if (meter_.joinable()) {
        meter_.join();
    }
}

} // namespace dictscribe::asr
