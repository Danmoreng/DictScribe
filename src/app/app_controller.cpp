#include "app/app_controller.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace dictscribe::app {

namespace {

constexpr auto kLiveCleanupDebounce = std::chrono::milliseconds(700);
constexpr auto kLiveCleanupMaximumDelay = std::chrono::milliseconds(2000);

std::string FileName(const std::filesystem::path& path) {
    return path.filename().string();
}

std::string AppendTranscript(const std::string& prefix, const std::string& text) {
    if (prefix.empty()) return text;
    if (text.empty()) return prefix;
    if (std::isspace(static_cast<unsigned char>(prefix.back())) ||
        std::isspace(static_cast<unsigned char>(text.front()))) {
        return prefix + text;
    }
    return prefix + " " + text;
}

} // namespace

AppController::~AppController() {
    std::string ignored;
    if (asr_.running()) {
        asr_.send({{"v", 1}, {"type", "shutdown"}, {"id", "ui-shutdown-asr"}}, ignored);
    }
    if (rewrite_.running()) {
        rewrite_.send({{"v", 1}, {"type", "shutdown"}, {"id", "ui-shutdown-rewrite"}}, ignored);
    }
    asr_.stop();
    rewrite_.stop();
}

bool AppController::start(const AppConfig& config) {
    {
        std::lock_guard lock(mutex_);
        state_ = {};
        state_.asr_model_name = FileName(config.asr_model);
        state_.rewrite_model_name = FileName(config.rewrite_model);
        state_.language = config.language;
    }

    std::string error;
    std::vector<std::string> asr_arguments = {
        "--stdio",
        "--model",
        config.asr_model.string(),
        "--protocol-version",
        "1",
    };
    if (config.use_gpu) {
        asr_arguments.push_back("--gpu");
    }
    if (!asr_.start(
            config.asr_worker.string(),
            asr_arguments,
            [this](const nlohmann::json& message) { handle_asr_message(message); },
            error)) {
        std::lock_guard lock(mutex_);
        set_error_locked(std::move(error));
        return false;
    }

    std::vector<std::string> rewrite_arguments = {
        "--stdio",
        "--model",
        config.rewrite_model.string(),
        "--protocol-version",
        "1",
    };
    if (config.use_gpu) {
        rewrite_arguments.insert(
            rewrite_arguments.end(), {"--gpu-layers", "99"});
    }
    if (!rewrite_.start(
            config.rewrite_worker.string(),
            rewrite_arguments,
            [this](const nlohmann::json& message) { handle_rewrite_message(message); },
            error)) {
        std::lock_guard lock(mutex_);
        set_error_locked(std::move(error));
        return false;
    }
    return true;
}

void AppController::set_startup_error(std::string message) {
    std::lock_guard lock(mutex_);
    set_error_locked(std::move(message));
}

void AppController::toggle_recording() {
    std::lock_guard lock(mutex_);
    if (state_.mode == DictationMode::Ready || state_.mode == DictationMode::Complete) {
        start_recording_locked(true);
        return;
    }

    if (state_.mode == DictationMode::Recording) {
        std::string error;
        state_.mode = DictationMode::Finalizing;
        state_.audio_rms = 0.0F;
        state_.audio_peak = 0.0F;
        state_.status = "Finalizing speech recognition...";
        if (!asr_.send({
                {"v", 1},
                {"type", "stop"},
                {"id", next_id_locked("stop")},
                {"sessionId", session_id_},
            }, error)) {
            set_error_locked(std::move(error));
        }
    }
}

bool AppController::start_recording_locked(bool clear_transcript) {
    if (clear_transcript) {
        state_.live_text.clear();
        state_.raw_final_text.clear();
        state_.rewritten_text.clear();
        state_.error.clear();
        session_prefix_.clear();
        language_restart_pending_ = false;
    }
    current_session_text_.clear();
    session_id_ = next_id_locked("session");
    state_.rewrite_in_progress = rewrite_in_flight_;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    finalization_waiting_ = false;
    pending_live_cleanup_ = false;
    state_.mode = DictationMode::StartingRecording;
    state_.status = clear_transcript
        ? "Opening the default microphone..."
        : "Restarting speech recognition with the selected language...";

    std::string error;
    if (!asr_.send({
            {"v", 1},
            {"type", "start"},
            {"id", next_id_locked("start")},
            {"sessionId", session_id_},
            {"language", state_.language},
        }, error)) {
        language_restart_pending_ = false;
        set_error_locked(std::move(error));
        return false;
    }
    return true;
}

bool AppController::stop_for_language_change_locked() {
    language_restart_pending_ = true;
    if (state_.mode == DictationMode::StartingRecording) {
        state_.status = "Waiting for the microphone before switching language...";
        return true;
    }
    if (state_.mode != DictationMode::Recording) return false;

    state_.mode = DictationMode::Finalizing;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.status = "Switching speech recognition language...";
    std::string error;
    if (!asr_.send({
            {"v", 1},
            {"type", "stop"},
            {"id", next_id_locked("language-stop")},
            {"sessionId", session_id_},
        }, error)) {
        language_restart_pending_ = false;
        set_error_locked(std::move(error));
        return false;
    }
    return true;
}

void AppController::cancel_recording() {
    std::lock_guard lock(mutex_);
    if (state_.mode != DictationMode::Recording &&
        state_.mode != DictationMode::StartingRecording) {
        return;
    }
    std::string error;
    state_.mode = DictationMode::Cancelling;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.status = "Cancelling dictation...";
    if (!asr_.send({
            {"v", 1},
            {"type", "cancel"},
            {"id", next_id_locked("cancel")},
            {"sessionId", session_id_},
        }, error)) {
        set_error_locked(std::move(error));
    }
}

void AppController::set_language(std::string language) {
    std::lock_guard lock(mutex_);
    if (!CanSetLanguage(state_)) {
        return;
    }
    if (language != "auto" && language != "de" && language != "en") {
        return;
    }
    if (language == state_.language) return;
    state_.language = std::move(language);
    if (state_.mode == DictationMode::Recording ||
        state_.mode == DictationMode::StartingRecording) {
        stop_for_language_change_locked();
        return;
    }
    if (state_.mode == DictationMode::Finalizing && language_restart_pending_) {
        state_.status = "Switching speech recognition language...";
        return;
    }
    if (state_.mode == DictationMode::Ready || state_.mode == DictationMode::Complete) {
        state_.status = "Language changed - ready for dictation";
    }
}

void AppController::tick() {
    std::lock_guard lock(mutex_);
    if (state_.mode != DictationMode::Recording ||
        !state_.rewrite_ready || rewrite_in_flight_ || !pending_live_cleanup_) {
        return;
    }
    if (std::chrono::steady_clock::now() >= live_cleanup_due_) {
        dispatch_rewrite_locked();
    }
}

AppSnapshot AppController::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void AppController::handle_asr_message(const nlohmann::json& message) {
    std::lock_guard lock(mutex_);
    const std::string type = message.value("type", "");
    if (type == "loading_model") {
        state_.status = "Loading speech and rewrite models...";
    } else if (type == "ready") {
        state_.asr_ready = true;
        update_ready_state_locked();
    } else if (type == "recording_started") {
        if (message.value("sessionId", "") != session_id_) return;
        state_.mode = DictationMode::Recording;
        state_.status = "Listening - live cleanup starts after a short pause";
        if (language_restart_pending_) {
            stop_for_language_change_locked();
        }
    } else if (type == "audio_level") {
        if (state_.mode == DictationMode::Recording &&
            message.value("sessionId", "") == session_id_) {
            state_.audio_rms = std::clamp(message.value("rms", 0.0F), 0.0F, 1.0F);
            state_.audio_peak = std::clamp(message.value("peak", 0.0F), 0.0F, 1.0F);
        }
    } else if (type == "transcript_update") {
        if (message.value("sessionId", "") != session_id_) return;
        current_session_text_ = message.value("text", "");
        update_transcript_locked(AppendTranscript(session_prefix_, current_session_text_));
    } else if (type == "recording_finalized") {
        if (message.value("sessionId", "") != session_id_) return;
        state_.audio_rms = 0.0F;
        state_.audio_peak = 0.0F;
        current_session_text_ = message.value("text", current_session_text_);
        state_.raw_final_text = AppendTranscript(session_prefix_, current_session_text_);
        update_transcript_locked(state_.raw_final_text);
        pending_live_cleanup_ = false;
        if (language_restart_pending_) {
            language_restart_pending_ = false;
            session_prefix_ = state_.raw_final_text;
            current_session_text_.clear();
            start_recording_locked(false);
            return;
        }
        if (state_.raw_final_text.empty()) {
            state_.mode = DictationMode::Complete;
            state_.status = "No speech recognized - press Enter to try again";
        } else if (rewrite_in_flight_ && active_rewrite_session_id_ == session_id_) {
            finalization_waiting_ = true;
            state_.mode = DictationMode::Finalizing;
            state_.status = "Waiting for the current live cleanup...";
        } else {
            finish_dictation_locked();
        }
    } else if (type == "recording_cancelled") {
        state_.mode = DictationMode::Ready;
        state_.audio_rms = 0.0F;
        state_.audio_peak = 0.0F;
        state_.status = "Cancelled - ready for another dictation";
        state_.live_text.clear();
        state_.raw_final_text.clear();
        state_.rewritten_text.clear();
        state_.rewrite_in_progress = false;
        finalization_waiting_ = false;
        language_restart_pending_ = false;
        pending_live_cleanup_ = false;
        session_id_.clear();
        session_prefix_.clear();
        current_session_text_.clear();
    } else if (type == "warning") {
        state_.status = message.value("message", "Audio processing warning");
    } else if (type == "error") {
        const std::string message_text = message.value("message", "Speech worker error");
        const bool recoverable = message.value("recoverable", false);
        const std::string code = message.value("code", "");
        if (recoverable && code == "MICROPHONE_UNAVAILABLE") {
            state_.mode = DictationMode::Ready;
            state_.audio_rms = 0.0F;
            state_.audio_peak = 0.0F;
            state_.status = "Microphone unavailable - check the default input and try again";
            state_.error = message_text;
            session_id_.clear();
        } else {
            set_error_locked(message_text);
        }
    }
}

void AppController::handle_rewrite_message(const nlohmann::json& message) {
    std::lock_guard lock(mutex_);
    const std::string type = message.value("type", "");
    if (type == "ready") {
        state_.rewrite_ready = true;
        update_ready_state_locked();
    } else if (type == "rewrite_completed") {
        if (!rewrite_in_flight_ || message.value("requestId", "") != active_rewrite_id_) {
            return;
        }
        const bool same_session = active_rewrite_session_id_ == session_id_;
        rewrite_in_flight_ = false;
        state_.rewrite_in_progress = false;
        active_rewrite_id_.clear();
        active_rewrite_session_id_.clear();
        if (!same_session) {
            if (state_.mode == DictationMode::Recording && pending_live_cleanup_) {
                live_cleanup_due_ = std::chrono::steady_clock::now();
            }
            return;
        }

        state_.rewritten_text = message.value("text", "");
        state_.error.clear();
        if (finalization_waiting_) {
            finish_dictation_locked();
        } else if (state_.mode == DictationMode::Recording) {
            state_.status = pending_live_cleanup_
                ? "Listening - newer speech is waiting for cleanup"
                : "Listening - live cleanup is up to date";
        }
    } else if (type == "error") {
        const std::string message_text = message.value("message", "Rewrite worker error");
        const bool recoverable = message.value("recoverable", false);
        const bool same_session = active_rewrite_session_id_ == session_id_;
        const bool request_failed = rewrite_in_flight_;
        if (request_failed) {
            rewrite_in_flight_ = false;
            state_.rewrite_in_progress = false;
            active_rewrite_id_.clear();
            active_rewrite_session_id_.clear();
        }
        if (!recoverable || message.value("code", "") == "WORKER_EXITED") {
            set_error_locked(message_text);
        } else if (!same_session) {
            return;
        } else if (finalization_waiting_) {
            finalization_waiting_ = false;
            state_.mode = DictationMode::Complete;
            state_.rewritten_text = state_.raw_final_text;
            state_.status = "Cleanup failed; showing the raw transcript";
            state_.error = message_text;
        } else if (state_.mode == DictationMode::Recording) {
            pending_live_cleanup_ = false;
            state_.status = "Listening - live cleanup failed; raw transcription continues";
            state_.error = message_text;
        }
    }
}

void AppController::update_ready_state_locked() {
    if (state_.asr_ready && state_.rewrite_ready && state_.mode == DictationMode::Starting) {
        state_.mode = DictationMode::Ready;
        state_.status = "Ready - press Enter or click Start dictation";
    } else if (!state_.asr_ready || !state_.rewrite_ready) {
        state_.status = "Loading speech and rewrite models...";
    }
}

void AppController::update_transcript_locked(std::string text) {
    if (text == state_.live_text) {
        return;
    }
    state_.live_text = std::move(text);
    const auto now = std::chrono::steady_clock::now();
    if (!pending_live_cleanup_) {
        live_cleanup_pending_since_ = now;
    }
    pending_live_cleanup_ = !state_.live_text.empty();
    live_cleanup_due_ = std::min(
        now + kLiveCleanupDebounce,
        live_cleanup_pending_since_ + kLiveCleanupMaximumDelay);
    if (state_.mode == DictationMode::Recording) {
        state_.status = rewrite_in_flight_
            ? "Listening - live cleanup is processing an earlier snapshot"
            : "Listening - live cleanup queued";
    }
}

bool AppController::dispatch_rewrite_locked() {
    if (rewrite_in_flight_ || !state_.rewrite_ready) {
        return false;
    }
    const std::string& text = state_.live_text;
    if (text.empty()) {
        return false;
    }

    active_rewrite_id_ = next_id_locked("live-rewrite");
    active_rewrite_session_id_ = session_id_;
    rewrite_in_flight_ = true;
    state_.rewrite_in_progress = true;
    pending_live_cleanup_ = false;
    state_.status = "Listening - cleaning the current transcript...";

    std::string error;
    if (!rewrite_.send({
            {"v", 1},
            {"type", "rewrite"},
            {"id", active_rewrite_id_},
            {"requestId", active_rewrite_id_},
            {"language", state_.language},
            {"text", text},
        }, error)) {
        rewrite_in_flight_ = false;
        state_.rewrite_in_progress = false;
        state_.status = "Listening - live cleanup unavailable";
        state_.error = std::move(error);
        active_rewrite_id_.clear();
        active_rewrite_session_id_.clear();
        return false;
    }
    return true;
}

void AppController::finish_dictation_locked() {
    finalization_waiting_ = false;
    pending_live_cleanup_ = false;
    state_.rewrite_in_progress = false;
    if (state_.rewritten_text.empty()) {
        state_.rewritten_text = state_.raw_final_text;
    }
    state_.mode = DictationMode::Complete;
    state_.status = "Dictation complete using the latest live cleanup";
}

void AppController::set_error_locked(std::string message) {
    state_.mode = DictationMode::Error;
    state_.audio_rms = 0.0F;
    state_.audio_peak = 0.0F;
    state_.status = "DictScribe needs attention";
    state_.error = std::move(message);
}

std::string AppController::next_id_locked(const char* prefix) {
    return std::string(prefix) + "-" + std::to_string(++request_sequence_);
}

bool CanToggle(const AppSnapshot& snapshot) {
    return snapshot.mode == DictationMode::Ready ||
        snapshot.mode == DictationMode::Complete ||
        snapshot.mode == DictationMode::Recording;
}

bool CanCancel(const AppSnapshot& snapshot) {
    return snapshot.mode == DictationMode::Recording ||
        snapshot.mode == DictationMode::StartingRecording;
}

bool CanSetLanguage(const AppSnapshot& snapshot) {
    return snapshot.mode == DictationMode::Starting ||
        snapshot.mode == DictationMode::Ready ||
        snapshot.mode == DictationMode::StartingRecording ||
        snapshot.mode == DictationMode::Recording ||
        snapshot.mode == DictationMode::Finalizing ||
        snapshot.mode == DictationMode::Complete ||
        snapshot.mode == DictationMode::Error;
}

const char* LanguageLabel(const AppSnapshot& snapshot) {
    if (snapshot.language == "de") return "Language: Deutsch";
    if (snapshot.language == "en") return "Language: English";
    return "Language: Auto";
}

const char* PrimaryButtonLabel(const AppSnapshot& snapshot) {
    switch (snapshot.mode) {
    case DictationMode::Recording:
        return "Stop dictation";
    case DictationMode::StartingRecording:
        return "Opening microphone...";
    case DictationMode::Finalizing:
        return "Finalizing transcript...";
    case DictationMode::Complete:
        return "Start another dictation";
    case DictationMode::Ready:
        return "Start dictation";
    case DictationMode::Error:
        return "Unavailable";
    case DictationMode::Starting:
        return "Loading models...";
    case DictationMode::Cancelling:
        return "Cancelling...";
    }
    return "Start dictation";
}

} // namespace dictscribe::app
